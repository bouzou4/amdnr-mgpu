// PCIe link of each card from AMD ADLX (amdadlx64.dll, loaded at run time). Windows' PnP link properties only
// see the card's own switch (x16 Gen5 on boards with one); the driver reports the slot's link.
// The vtable layouts below follow ADLX's C bindings (ISystem.h / ISystem1.h, MIT, GPUOpen ADLX SDK).
#include "MgpuCommon.h"
#include "MgpuCore.h"

namespace Mgpu
{
namespace
{
using R = int; // ADLX_RESULT, 0 = OK
struct Gpu1;
struct Gpu1Vtbl
{
    long(__stdcall* Acquire)(Gpu1*);
    long(__stdcall* Release)(Gpu1*);
    R(__stdcall* QueryInterface)(Gpu1*, const wchar_t*, void**);
    void* gpu[15]; // VendorId .. SubSystemVendorId
    R(__stdcall* UniqueId)(Gpu1*, int*);
    R(__stdcall* PCIBusType)(Gpu1*, int*);
    R(__stdcall* PCIBusLaneWidth)(Gpu1*, unsigned*);
};
struct Gpu1
{
    const Gpu1Vtbl* v;
};
struct GpuList;
struct GpuListVtbl
{
    long(__stdcall* Acquire)(GpuList*);
    long(__stdcall* Release)(GpuList*);
    R(__stdcall* QueryInterface)(GpuList*, const wchar_t*, void**);
    unsigned(__stdcall* Size)(GpuList*);
    uint8_t(__stdcall* Empty)(GpuList*);
    unsigned(__stdcall* Begin)(GpuList*);
    unsigned(__stdcall* End)(GpuList*);
    void* list[4]; // At, Clear, Remove_Back, Add_Back
    R(__stdcall* AtGpu)(GpuList*, unsigned, Gpu1**);
};
struct GpuList
{
    const GpuListVtbl* v;
};
struct System;
struct SystemVtbl
{
    void* hybrid;
    R(__stdcall* GetGPUs)(System*, GpuList**);
};
struct System
{
    const SystemVtbl* v;
};

// ADLX_PCI_BUS_TYPE: 3 PCIE (gen 1) .. 7 PCIE_5_0
int GenOf(int busType) { return busType >= 3 && busType <= 7 ? busType - 2 : 0; }

std::atomic<bool> s_started { false };

void Query()
{
    HMODULE m = LoadLibraryW(L"amdadlx64.dll");
    if (!m)
        return Log(0, "PCIe link: no ADLX (not an AMD driver): the link is inferred from the measured transfer rate");
    using QueryFull = R(__cdecl*)(uint64_t*);
    using Init = R(__cdecl*)(uint64_t, System**);
    auto qv = (QueryFull) GetProcAddress(m, "ADLXQueryFullVersion");
    auto init = (Init) GetProcAddress(m, "ADLXInitialize");
    uint64_t ver = 0;
    System* sys = nullptr;
    if (!qv || !init || qv(&ver) != 0 || init(ver, &sys) != 0 || !sys)
        return Log(1, "PCIe link: ADLX did not initialise: the link is inferred from the measured transfer rate");
    Stats st;
    {
        std::lock_guard g(StatsMutex());
        st = MutableStats();
    }
    GpuList* list = nullptr;
    if (sys->v->GetGPUs(sys, &list) == 0 && list)
    {
        for (unsigned i = list->v->Begin(list); i != list->v->End(list); ++i)
        {
            Gpu1* gpu = nullptr;
            if (list->v->AtGpu(list, i, &gpu) != 0 || !gpu)
                continue;
            Gpu1* g1 = nullptr;
            int uid = -1, type = 0;
            unsigned lanes = 0;
            if (gpu->v->QueryInterface(gpu, L"IADLXGPU1", (void**) &g1) == 0 && g1)
            {
                g1->v->UniqueId(g1, &uid);
                g1->v->PCIBusType(g1, &type);
                g1->v->PCIBusLaneWidth(g1, &lanes);
                g1->v->Release(g1);
            }
            gpu->v->Release(gpu);
            const int bus = uid >= 0 ? uid >> 8 : -1; // UniqueId = bus << 8 | device << 3 | function
            if (bus < 0 || !GenOf(type) || !lanes)
                continue;
            std::lock_guard g(StatsMutex());
            auto& s = MutableStats();
            for (AdapterView* a : { &s.render, &s.display })
                if (a->bus == bus)
                {
                    a->pcieGen = GenOf(type);
                    a->pcieLanes = (int) lanes;
                    Log(0, std::format("PCIe link of the {} card (bus {}): Gen{} x{} (ADLX), {:.1f} GB/s per direction",
                                       a == &s.render ? "render" : "display", bus, a->pcieGen, a->pcieLanes,
                                       LinkGBs(a->pcieGen, a->pcieLanes)));
                }
        }
        list->v->Release(list);
    }
    // ADLX stays initialised and loaded: terminating it while the driver creates a device crashed the process
}
} // namespace

double LinkGBs(int gen, int lanes)
{
    static const double perLane[] = { 0, 0.25, 0.5, 0.985, 1.969, 3.938 };
    return gen >= 1 && gen <= 5 ? perLane[gen] * lanes : 0;
}

// Once, on the caller's thread (the topology is built before the game's device): not beside a device creation
void StartLinkQuery()
{
    if (!s_started.exchange(true))
        Query();
}
} // namespace Mgpu
