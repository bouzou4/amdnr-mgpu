#include "MgpuCommon.h"
#include "MgpuCore.h"

#include <wrl/client.h>
#include <d3dkmthk.h>
#include <mutex>
#include <vector>
#include <format>

#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

namespace Mgpu
{
struct Topology
{
    ComPtr<IDXGIAdapter1> render, display;
    AdapterView rv, dv;
    ComPtr<ID3D12Device> displayDevice;
    ComPtr<ID3D12CommandQueue> displayQueue;
};

static Settings s_settings;
static Env s_env;
static std::once_flag s_topoOnce;
static Topology* s_topo = nullptr;
static std::mutex s_devMutex;
static thread_local int t_internal = 0;
static bool s_redirected = false;
static Stats s_stats;

void Configure(const Settings& s, const Env& e)
{
    s_settings = s;
    s_env = e;
}
const Settings& CurrentSettings() { return s_settings; }
const Env& CurrentEnv() { return s_env; }
Stats& MutableStats() { return s_stats; }

void Log(int level, const std::string& s)
{
    if (s_env.log)
        s_env.log(level, "[MGPU] " + s);
}

static int PciBus(LUID luid)
{
    D3DKMT_OPENADAPTERFROMLUID open {};
    open.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0)
        return -1;
    D3DKMT_ADAPTERADDRESS addr {};
    D3DKMT_QUERYADAPTERINFO q {};
    q.hAdapter = open.hAdapter;
    q.Type = KMTQAITYPE_ADAPTERADDRESS;
    q.pPrivateDriverData = &addr;
    q.PrivateDriverDataSize = sizeof(addr);
    int bus = -1;
    if (D3DKMTQueryAdapterInfo(&q) == 0 && addr.BusNumber != 0xFFFFFFFF)
        bus = (int) addr.BusNumber;
    D3DKMT_CLOSEADAPTER close { open.hAdapter };
    D3DKMTCloseAdapter(&close);
    return bus;
}

static std::string Narrow(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void BuildTopology()
{
    if (!s_env.createFactory)
        return;
    struct Cand
    {
        ComPtr<IDXGIAdapter1> a;
        AdapterView v;
    };
    std::vector<Cand> c;
    auto scan = [&]
    {
        ComPtr<IDXGIFactory1> f;
        f.Attach(s_env.createFactory());
        if (!f)
        {
            Log(2, "no DXGI factory");
            return;
        }
        ComPtr<IDXGIAdapter1> a;
        HRESULT he = S_OK;
        UINT i = 0;
        for (; (he = f->EnumAdapters1(i, &a)) == S_OK; ++i, a.Reset())
        {
            DXGI_ADAPTER_DESC1 d {};
            HRESULT hd = a->GetDesc1(&d);
            if (hd != S_OK || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            {
                Log(0, std::format("adapter {} skipped (desc {:X}, flags {:X})", i, (uint32_t) hd, d.Flags));
                continue;
            }
            Cand x { a, {} };
            x.v.name = Narrow(d.Description);
            x.v.luid = d.AdapterLuid;
            x.v.vram = d.DedicatedVideoMemory;
            x.v.bus = PciBus(d.AdapterLuid);
            ComPtr<IDXGIOutput> o;
            while (a->EnumOutputs(x.v.outputs, &o) == S_OK)
            {
                o.Reset();
                x.v.outputs++;
            }
            Log(0, std::format("adapter {}: '{}' bus {} outputs {} luid {:08X}:{:08X}", i, x.v.name, x.v.bus,
                               x.v.outputs, (uint32_t) d.AdapterLuid.HighPart, d.AdapterLuid.LowPart));
            if (x.v.bus >= 0)
                c.push_back(x);
        }
        Log(0, std::format("{} adapters enumerated (last {:X})", i, (uint32_t) he));
    };
    if (s_env.noSpoof)
        s_env.noSpoof(scan);
    else
        scan();

    Cand *r = nullptr, *d = nullptr;
    for (auto& x : c)
    {
        if (s_settings.renderBus >= 0 ? x.v.bus == s_settings.renderBus : (x.v.outputs == 0 && !r))
            r = &x;
        if (s_settings.displayBus >= 0 ? x.v.bus == s_settings.displayBus : (x.v.outputs > 0 && !d))
            d = &x;
    }
    if (!r || !d || r == d)
    {
        Log(2, std::format("no usable pair (render {}, display {}): single-GPU", r ? r->v.bus : -1,
                           d ? d->v.bus : -1));
        return;
    }
    auto* t = new Topology();
    t->render = r->a;
    t->display = d->a;
    t->rv = r->v;
    t->dv = d->v;
    s_topo = t;
    s_stats.topology = true;
    s_stats.render = t->rv;
    s_stats.display = t->dv;
    StartLoadSampler();
    StartLinkQuery();
    Log(0, std::format("render card bus {} '{}', display card bus {} '{}'", t->rv.bus, t->rv.name, t->dv.bus,
                       t->dv.name));
}

Topology* GetTopology()
{
    if (!s_settings.enabled)
        return nullptr;
    std::call_once(s_topoOnce, BuildTopology);
    return s_topo;
}

bool InternalCreate() { return t_internal > 0; }
bool Redirected() { return s_redirected; }

IUnknown* RedirectAdapter(IUnknown* adapter)
{
    if (!s_settings.enabled || t_internal > 0)
        return adapter;
    auto* t = GetTopology();
    if (!t)
        return adapter;
    if (adapter)
    {
        ComPtr<IDXGIAdapter> a;
        DXGI_ADAPTER_DESC d {};
        if (adapter->QueryInterface(IID_PPV_ARGS(&a)) != S_OK || a->GetDesc(&d) != S_OK)
            return adapter;
        if (d.AdapterLuid.LowPart == t->rv.luid.LowPart && d.AdapterLuid.HighPart == t->rv.luid.HighPart)
        {
            s_redirected = true;
            return adapter;
        }
        if (d.AdapterLuid.LowPart != t->dv.luid.LowPart || d.AdapterLuid.HighPart != t->dv.luid.HighPart)
            return adapter; // some other adapter (WARP, iGPU): not ours
    }
    if (!s_redirected)
        Log(0, std::format("game device redirected to bus {}", t->rv.bus));
    s_redirected = true;
    std::lock_guard g(StatsMutex());
    s_stats.redirected = true;
    return t->render.Get();
}

static bool EnsureDisplayDevice()
{
    std::lock_guard g(s_devMutex);
    auto* t = GetTopology();
    if (!t)
        return false;
    if (t->displayDevice)
        return true;
    if (!s_env.createDevice)
        return false;
    if (CurrentSettings().dred)
    {
        // Process wide, for devices created from here on
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
        {
            if (CurrentSettings().dred == 1)
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            Log(0, std::format("DRED on for the display device ({})", CurrentSettings().dred == 1 ? "breadcrumbs, page faults" : "page faults"));
        }
    }
    ++t_internal;
    HRESULT hr = s_env.createDevice(t->display.Get(), IID_PPV_ARGS(&t->displayDevice));
    --t_internal;
    if (FAILED(hr))
    {
        Log(2, std::format("display device creation failed {:X}", (uint32_t) hr));
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC q {};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(t->displayDevice->CreateCommandQueue(&q, IID_PPV_ARGS(&t->displayQueue))))
    {
        q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(hr = t->displayDevice->CreateCommandQueue(&q, IID_PPV_ARGS(&t->displayQueue))))
        {
            Log(2, std::format("display queue creation failed {:X}", (uint32_t) hr));
            t->displayDevice.Reset();
            return false;
        }
    }
    t->displayQueue->SetName(L"Mgpu display queue");
    Log(0, std::format("display device created on bus {}", t->dv.bus));
    return true;
}

ID3D12Device* DisplayDevice() { return EnsureDisplayDevice() ? s_topo->displayDevice.Get() : nullptr; }

void LogDeviceRemoved(ID3D12Device* device, const char* where)
{
    static std::mutex m;
    static std::vector<ID3D12Device*> done;
    std::lock_guard g(m);
    if (device == nullptr || std::find(done.begin(), done.end(), device) != done.end())
        return;
    done.push_back(device);
    Log(2, std::format("{}: device removed reason {:X}", where, (uint32_t) device->GetDeviceRemovedReason()));
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return;
    D3D12_DRED_PAGE_FAULT_OUTPUT pf {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)))
    {
        Log(2, std::format("DRED page fault at {:X}", pf.PageFaultVA));
        for (auto* n = pf.pHeadExistingAllocationNode; n; n = n->pNext)
            Log(2, std::format("  existing allocation {} type {}", n->ObjectNameA ? n->ObjectNameA : "?", (int) n->AllocationType));
        for (auto* n = pf.pHeadRecentFreedAllocationNode; n; n = n->pNext)
            Log(2, std::format("  freed allocation {} type {}", n->ObjectNameA ? n->ObjectNameA : "?", (int) n->AllocationType));
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc {};
    if (FAILED(dred->GetAutoBreadcrumbsOutput(&bc)))
        return;
    int lists = 0;
    for (auto* n = bc.pHeadAutoBreadcrumbNode; n && lists < 24; n = n->pNext)
    {
        const UINT done2 = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        if (done2 >= n->BreadcrumbCount)
            continue; // finished
        ++lists;
        std::string ops;
        for (UINT i = done2 > 2 ? done2 - 2 : 0; i < n->BreadcrumbCount && i < done2 + 4; i++)
            ops += std::format("{}{}{} ", i == done2 ? "[" : "", (int) n->pCommandHistory[i], i == done2 ? "]" : "");
        Log(2, std::format("DRED list {} queue {}: {}/{} ops done, around: {}",
                           n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?",
                           n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "?", done2, n->BreadcrumbCount, ops));
    }
}
ID3D12CommandQueue* DisplayQueue() { return EnsureDisplayDevice() ? s_topo->displayQueue.Get() : nullptr; }

IDXGIAdapter1* DisplayAdapter() { return s_topo ? s_topo->display.Get() : nullptr; }
LUID RenderLuid() { return s_topo ? s_topo->rv.luid : LUID {}; }

bool NrOnDisplay()
{
    return s_settings.enabled && s_settings.presenter && s_redirected && s_topo && !s_stats.fallback && s_env.nrWanted &&
           s_env.nrWanted();
}

Stats GetStats()
{
    Stats s;
    {
        std::lock_guard g(StatsMutex());
        s = s_stats;
    }
    s.nrOnDisplay = NrOnDisplay();
    if (s_env.nrStatus && s.nrOnDisplay)
        s.nrStatus = s_env.nrStatus();
    return s;
}
} // namespace Mgpu
