#include "pch.h"
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
            return;
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; ++i, a.Reset())
        {
            DXGI_ADAPTER_DESC1 d {};
            if (a->GetDesc1(&d) != S_OK || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                continue;
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
            Log(0, std::format("adapter {} '{}' bus {} outputs {} luid {:08X}:{:08X}", i, x.v.name, x.v.bus,
                               x.v.outputs, (uint32_t) d.AdapterLuid.HighPart, d.AdapterLuid.LowPart));
            if (x.v.bus >= 0)
                c.push_back(x);
        }
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
    using PFN = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto fn = (PFN) GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateDevice");
    if (!fn)
        return false;
    ++t_internal;
    HRESULT hr = fn(t->display.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&t->displayDevice));
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
ID3D12CommandQueue* DisplayQueue() { return EnsureDisplayDevice() ? s_topo->displayQueue.Get() : nullptr; }

bool NrOnDisplay()
{
    return s_settings.enabled && s_settings.presenter && s_redirected && s_topo && s_env.nrWanted &&
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
