// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Legacy mode, game side: a 32-bit d3d9.dll proxy. The game's D3D9(Ex) device runs on D3D9On12 over a D3D12 device
// of ours; at each present the back buffer goes to the x64 NR host through a shared texture and fence and back.
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <d3d9on12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include "../LegacyShared.h"

using Microsoft::WRL::ComPtr;

namespace
{
HMODULE g_self = nullptr;
HMODULE g_sys = nullptr;
std::wstring g_dir, g_ini, g_logPath;
FILE* g_log = nullptr;
std::mutex g_logMutex;

void Log(const char* fmt, ...)
{
    std::lock_guard lock(g_logMutex);
    if (g_log == nullptr)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

int IniInt(const wchar_t* key, int def) { return GetPrivateProfileIntW(L"Legacy", key, def, g_ini.c_str()); }
std::wstring IniStr(const wchar_t* key)
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"Legacy", key, L"", buf, MAX_PATH * 2, g_ini.c_str());
    return buf;
}

// D3D12 side
ComPtr<ID3D12Device> g_dev;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12Fence> g_shared;  // with the host, see LegacyShared.h
ComPtr<ID3D12Fence> g_local;   // return fence for 9On12
uint64_t g_localValue = 0;
HANDLE g_sharedHandle = nullptr, g_event = nullptr;
ComPtr<ID3D12CommandAllocator> g_alloc[2];
ComPtr<ID3D12GraphicsCommandList> g_list;
uint64_t g_allocValue[2] = {};
uint32_t g_allocNext = 0;
ComPtr<ID3D12Resource> g_frame;
HANDLE g_frameHandle = nullptr;
uint32_t g_generation = 0;
uint64_t g_n = 0;            // frames signalled to the host
bool g_stalled = false;      // the host missed a deadline: wait for it before the next frame

HANDLE g_mapping = nullptr;
Legacy::Block* g_block = nullptr;
HANDLE g_host = nullptr;
bool g_enabled = true, g_on12 = false;
uint32_t g_timeoutMs = 250;
std::mutex g_mutex;
std::wstring g_bypassPath; // <DumpDir>\proxy_bypass.trigger: while it exists frames skip the host (fps baseline)
bool g_bypass = false;

// Stats
uint64_t g_presents = 0, g_sent = 0, g_returned = 0, g_timeouts = 0, g_skipped = 0;
LARGE_INTEGER g_freq {}, g_lastStat {};
double g_waitMsSum = 0;

bool InitD3D12()
{
    ComPtr<IDXGIFactory4> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
        return false;
    ComPtr<IDXGIAdapter1> adapter;
    const int want = IniInt(L"Adapter", 0); // DXGI index; the game's D3D9 default adapter is the primary one
    if (FAILED(f->EnumAdapters1(want, &adapter)))
        return false;
    DXGI_ADAPTER_DESC1 ad {};
    adapter->GetDesc1(&ad);
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev))))
    {
        Log("D3D12CreateDevice failed on %ls", ad.Description);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue))))
        return false;
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_shared));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_local));
    for (auto& a : g_alloc)
        g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0].Get(), nullptr, IID_PPV_ARGS(&g_list));
    if (!g_shared || !g_local || !g_list)
        return false;
    g_list->Close();
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    const DWORD pid = GetCurrentProcessId();
    wchar_t name[128];
    swprintf_s(name, Legacy::kFenceName, pid);
    if (FAILED(g_dev->CreateSharedHandle(g_shared.Get(), nullptr, GENERIC_ALL, name, &g_sharedHandle)))
    {
        Log("fence share failed");
        return false;
    }
    swprintf_s(name, Legacy::kBlockName, pid);
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Legacy::Block), name);
    g_block = g_mapping ? (Legacy::Block*) MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Legacy::Block))
                        : nullptr;
    if (g_block == nullptr)
        return false;
    ZeroMemory(g_block, sizeof(*g_block));
    g_block->magic = Legacy::kMagic;
    g_block->version = Legacy::kVersion;
    g_block->gamePid = pid;
    const LUID luid = g_dev->GetAdapterLuid();
    g_block->adapterLuidLow = luid.LowPart;
    g_block->adapterLuidHigh = luid.HighPart;
    Log("D3D12 device on %ls (LUID %08x:%08x)", ad.Description, luid.HighPart, luid.LowPart);
    return true;
}

void StartHost()
{
    const std::wstring exe = IniStr(L"HostExe");
    if (exe.empty())
    {
        Log("no HostExe: frames are not processed");
        return;
    }
    std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" \"" + g_ini + L"\"";
    STARTUPINFOW si { sizeof(si) };
    PROCESS_INFORMATION pi {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi))
    {
        Log("host start failed (%lu): %ls", GetLastError(), exe.c_str());
        return;
    }
    CloseHandle(pi.hThread);
    g_host = pi.hProcess;
    Log("host started, pid %lu", pi.dwProcessId);
}

bool HostAlive()
{
    return g_host != nullptr && WaitForSingleObject(g_host, 0) == WAIT_TIMEOUT && g_block->hostReady != 0;
}

bool EnsureFrame(const D3D12_RESOURCE_DESC& bb)
{
    if (g_frame)
    {
        const auto d = g_frame->GetDesc();
        if (d.Width == bb.Width && d.Height == bb.Height && d.Format == bb.Format)
            return true;
    }
    if (g_frameHandle)
        CloseHandle(g_frameHandle), g_frameHandle = nullptr;
    g_frame.Reset();
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = bb.Width;
    d.Height = bb.Height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = bb.Format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&g_frame));
    if (FAILED(hr))
    {
        Log("frame texture %llux%u format %u failed: %08x", d.Width, d.Height, d.Format, hr);
        return false;
    }
    ++g_generation;
    wchar_t name[128];
    swprintf_s(name, Legacy::kFrameName, GetCurrentProcessId(), g_generation);
    hr = g_dev->CreateSharedHandle(g_frame.Get(), nullptr, GENERIC_ALL, name, &g_frameHandle);
    if (FAILED(hr))
    {
        Log("frame share failed: %08x", hr);
        g_frame.Reset();
        return false;
    }
    g_block->width = (uint32_t) d.Width;
    g_block->height = d.Height;
    g_block->format = d.Format;
    g_block->colorSpace = 1;
    g_block->reset = 1;
    MemoryBarrier();
    g_block->generation = g_generation;
    Log("frame texture %llux%u format %u, generation %u", d.Width, d.Height, d.Format, g_generation);
    return true;
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}

// Copies src -> dst on our queue (both COMMON at the boundaries)
void Copy(ID3D12Resource* dst, ID3D12Resource* src)
{
    const uint32_t i = g_allocNext++ & 1;
    if (g_local->GetCompletedValue() < g_allocValue[i])
    {
        g_local->SetEventOnCompletion(g_allocValue[i], g_event);
        WaitForSingleObject(g_event, 1000);
    }
    g_alloc[i]->Reset();
    g_list->Reset(g_alloc[i].Get(), nullptr);
    Barrier(g_list.Get(), src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(g_list.Get(), dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->CopyResource(dst, src);
    Barrier(g_list.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_list.Get(), dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    g_list->Close();
    ID3D12CommandList* l[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, l);
    g_allocValue[i] = ++g_localValue;
    g_queue->Signal(g_local.Get(), g_localValue);
}

void Stats()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_lastStat.QuadPart == 0)
        g_lastStat = now;
    const double s = double(now.QuadPart - g_lastStat.QuadPart) / double(g_freq.QuadPart);
    if (s < 10.0)
        return;
    const uint64_t frames = g_presents;
    static uint64_t lastFrames = 0, lastReturned = 0;
    const double fps = double(frames - lastFrames) / s;
    const uint64_t ret = g_returned - lastReturned;
    Log("pacing: %.1f fps, %llu presents, %llu returned from the host (%llu in window), %llu timeouts, %llu skipped, "
        "host wait %.2f ms/frame, host %s state %d gpu %.2f ms, of %.2f ms, flk %.5f / %.5f, %s",
        fps, frames, g_returned, ret, g_timeouts, g_skipped, ret ? g_waitMsSum / double(ret) : 0.0,
        HostAlive() ? "up" : "down", g_block ? g_block->hostState : -1, g_block ? g_block->hostMs : 0.f,
        g_block ? g_block->ofMs : 0.f, g_block ? g_block->flkStatic : 0.f, g_block ? g_block->flkMc : 0.f,
        g_block ? g_block->status : "");
    lastFrames = frames;
    lastReturned = g_returned;
    g_waitMsSum = 0;
    g_lastStat = now;
}

void OnPresent(IDirect3DDevice9* dev)
{
    std::lock_guard lock(g_mutex);
    ++g_presents;
    Stats();
    if (!g_bypassPath.empty() && g_presents % 30 == 0)
    {
        const bool b = GetFileAttributesW(g_bypassPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (b != g_bypass)
            Log("bypass %s", b ? "on" : "off");
        if (!b && g_bypass && g_block)
            g_block->reset = 1;
        g_bypass = b;
    }
    if (!g_on12 || !g_enabled || g_bypass || !HostAlive())
        return;
    if (g_stalled)
    {
        if (g_shared->GetCompletedValue() < 2 * g_n)
        {
            ++g_skipped;
            return;
        }
        g_stalled = false;
    }
    ComPtr<IDirect3DDevice9On12> d12;
    if (FAILED(dev->QueryInterface(IID_IDirect3DDevice9On12, (void**) d12.GetAddressOf())))
        return;
    ComPtr<IDirect3DSurface9> bb;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
    ComPtr<ID3D12Resource> res;
    HRESULT hr = d12->UnwrapUnderlyingResource(bb.Get(), g_queue.Get(), IID_PPV_ARGS(&res));
    if (FAILED(hr))
    {
        static int n = 0;
        if (n++ < 5)
            Log("UnwrapUnderlyingResource failed: %08x", hr);
        return;
    }
    const auto desc = res->GetDesc();
    static bool logged = false;
    if (!logged)
        logged = true, Log("back buffer %llux%u format %u samples %u flags %x", desc.Width, desc.Height, desc.Format,
                           desc.SampleDesc.Count, desc.Flags);
    if (desc.SampleDesc.Count == 1 && EnsureFrame(desc))
    {
        Copy(g_frame.Get(), res.Get());
        ++g_n;
        g_block->frame = g_n;
        MemoryBarrier();
        g_queue->Signal(g_shared.Get(), 2 * g_n - 1);
        ++g_sent;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        bool back = g_shared->GetCompletedValue() >= 2 * g_n;
        if (!back && SUCCEEDED(g_shared->SetEventOnCompletion(2 * g_n, g_event)))
            back = WaitForSingleObject(g_event, g_timeoutMs) == WAIT_OBJECT_0;
        QueryPerformanceCounter(&t1);
        if (back)
        {
            Copy(res.Get(), g_frame.Get());
            ++g_returned;
            g_waitMsSum += 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(g_freq.QuadPart);
        }
        else
        {
            g_stalled = true;
            if (g_timeouts++ < 10)
                Log("frame %llu: host did not answer within %u ms", g_n, g_timeoutMs);
        }
    }
    ++g_localValue;
    g_queue->Signal(g_local.Get(), g_localValue);
    ID3D12Fence* f = g_local.Get();
    UINT64 v = g_localValue;
    hr = d12->ReturnUnderlyingResource(bb.Get(), 1, &v, &f);
    if (FAILED(hr))
        Log("ReturnUnderlyingResource failed: %08x", hr);
}

// Vtable patching (one vtable per implementation class)
template <typename T> void Patch(void** vtbl, int index, T hook, T* orig)
{
    if (vtbl[index] == (void*) hook)
        return;
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = (T) vtbl[index];
    vtbl[index] = (void*) hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
}

using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PFN_PresentEx = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*,
                                                  DWORD);
using PFN_Reset = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PFN_ResetEx = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using PFN_CreateDevice = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using PFN_CreateDeviceEx = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                       D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
PFN_Present o_Present = nullptr;
PFN_PresentEx o_PresentEx = nullptr;
PFN_Reset o_Reset = nullptr;
PFN_ResetEx o_ResetEx = nullptr;
PFN_CreateDevice o_CreateDevice = nullptr;
PFN_CreateDeviceEx o_CreateDeviceEx = nullptr;
std::atomic<uint32_t> g_firstCalls { 0 };

HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
    if (!(g_firstCalls.fetch_or(1) & 1))
        Log("first Present");
    OnPresent(d);
    return o_Present(d, a, b, w, r);
}
HRESULT STDMETHODCALLTYPE hkPresentEx(IDirect3DDevice9Ex* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r,
                                      DWORD f)
{
    if (!(g_firstCalls.fetch_or(2) & 2))
        Log("first PresentEx (flags %lx)", f);
    OnPresent(d);
    return o_PresentEx(d, a, b, w, r, f);
}

void FixParams(D3DPRESENT_PARAMETERS* p, const char* where)
{
    if (p == nullptr)
        return;
    Log("%s: %ux%u format %u msaa %u windowed %d swap %u interval %x buffers %u", where, p->BackBufferWidth,
        p->BackBufferHeight, p->BackBufferFormat, p->MultiSampleType, p->Windowed, p->SwapEffect,
        p->PresentationInterval, p->BackBufferCount);
    if (!g_on12)
        return;
    if (IniInt(L"ForceNoMsaa", 1) && p->MultiSampleType != D3DMULTISAMPLE_NONE)
        p->MultiSampleType = D3DMULTISAMPLE_NONE, p->MultiSampleQuality = 0, Log("  MSAA off (NR needs one sample)");
    // BGRX has no typed UAV: the host writes the frame texture
    if (p->BackBufferFormat == D3DFMT_X8R8G8B8 || p->BackBufferFormat == D3DFMT_UNKNOWN)
        p->BackBufferFormat = D3DFMT_A8R8G8B8, Log("  back buffer A8R8G8B8");
}

HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* p)
{
    FixParams(p, "Reset");
    return o_Reset(d, p);
}
HRESULT STDMETHODCALLTYPE hkResetEx(IDirect3DDevice9Ex* d, D3DPRESENT_PARAMETERS* p, D3DDISPLAYMODEEX* m)
{
    FixParams(p, "ResetEx");
    return o_ResetEx(d, p, m);
}

void HookDevice(IDirect3DDevice9* dev, bool ex)
{
    void** vt = *(void***) dev;
    Patch(vt, 16, &hkReset, &o_Reset);
    Patch(vt, 17, &hkPresent, &o_Present);
    if (ex)
    {
        Patch(vt, 121, &hkPresentEx, &o_PresentEx);
        Patch(vt, 132, &hkResetEx, &o_ResetEx);
    }
    static bool started = false;
    if (!started && g_on12)
        started = true, StartHost();
}

HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                         D3DPRESENT_PARAMETERS* p, IDirect3DDevice9** out)
{
    FixParams(p, "CreateDevice");
    HRESULT hr = o_CreateDevice(d3d, a, t, w, f, p, out);
    Log("CreateDevice adapter %u: %08x", a, hr);
    if (SUCCEEDED(hr) && out && *out)
    {
        ComPtr<IDirect3DDevice9Ex> ex;
        (*out)->QueryInterface(IID_PPV_ARGS(&ex));
        HookDevice(*out, ex != nullptr);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateDeviceEx(IDirect3D9Ex* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                           D3DPRESENT_PARAMETERS* p, D3DDISPLAYMODEEX* m, IDirect3DDevice9Ex** out)
{
    FixParams(p, "CreateDeviceEx");
    HRESULT hr = o_CreateDeviceEx(d3d, a, t, w, f, p, m, out);
    Log("CreateDeviceEx adapter %u: %08x", a, hr);
    if (SUCCEEDED(hr) && out && *out)
        HookDevice(*out, true);
    return hr;
}

void HookD3D(IDirect3D9* d3d, bool ex)
{
    if (d3d == nullptr)
        return;
    void** vt = *(void***) d3d;
    Patch(vt, 16, &hkCreateDevice, &o_CreateDevice);
    if (ex)
        Patch(vt, 20, &hkCreateDeviceEx, &o_CreateDeviceEx);
}

void Init()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    QueryPerformanceFrequency(&g_freq);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/"));
    g_ini = g_dir + L"\\lmxxf_legacy.ini";
    std::wstring logDir = IniStr(L"LogDir");
    if (logDir.empty())
        logDir = g_dir;
    g_logPath = logDir + L"\\lmxxf_legacy_proxy.log";
    _wfopen_s(&g_log, g_logPath.c_str(), L"w");
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    g_sys = LoadLibraryW((std::wstring(sys) + L"\\d3d9.dll").c_str());
    g_enabled = IniInt(L"Enabled", 1) != 0;
    g_timeoutMs = (uint32_t) IniInt(L"TimeoutMs", 250);
    {
        wchar_t d[MAX_PATH] = {};
        GetPrivateProfileStringW(L"Host", L"DumpDir", L"", d, MAX_PATH, g_ini.c_str());
        if (d[0])
            g_bypassPath = std::wstring(d) + L"\\proxy_bypass.trigger";
    }
    g_on12 = IniInt(L"NineOn12", 1) != 0 && g_sys && GetProcAddress(g_sys, "Direct3DCreate9On12Ex") && InitD3D12();
    Log("lmxxf legacy proxy: pid %lu, system d3d9 %p, 9On12 %d, NR %d", GetCurrentProcessId(), g_sys, g_on12,
        g_enabled);
}

D3D9ON12_ARGS Args()
{
    D3D9ON12_ARGS a {};
    a.Enable9On12 = TRUE;
    a.pD3D12Device = g_dev.Get();
    a.ppD3D12Queues[0] = g_queue.Get();
    a.NumQueues = 1;
    return a;
}

template <typename T> T Sys(const char* name) { return g_sys ? (T) GetProcAddress(g_sys, name) : nullptr; }
} // namespace

extern "C"
{
IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    Init();
    IDirect3D9* d = nullptr;
    if (g_on12)
    {
        auto a = Args();
        d = Sys<PFN_Direct3DCreate9On12>("Direct3DCreate9On12")(sdk, &a, 1);
    }
    else if (auto f = Sys<IDirect3D9*(WINAPI*)(UINT)>("Direct3DCreate9"))
        d = f(sdk);
    Log("Direct3DCreate9(%u): %p", sdk, d);
    HookD3D(d, false);
    return d;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    Init();
    HRESULT hr = E_FAIL;
    if (g_on12)
    {
        auto a = Args();
        hr = Sys<PFN_Direct3DCreate9On12Ex>("Direct3DCreate9On12Ex")(sdk, &a, 1, out);
    }
    else if (auto f = Sys<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>("Direct3DCreate9Ex"))
        hr = f(sdk, out);
    Log("Direct3DCreate9Ex(%u): %08x", sdk, hr);
    if (SUCCEEDED(hr) && out)
        HookD3D(*out, true);
    return hr;
}

// Pass-through exports
void WINAPI D3DPERF_SetOptions(DWORD o)
{
    Init();
    if (auto f = Sys<void(WINAPI*)(DWORD)>("D3DPERF_SetOptions"))
        f(o);
}
int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n)
{
    auto f = Sys<int(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_BeginEvent");
    return f ? f(c, n) : 0;
}
int WINAPI D3DPERF_EndEvent()
{
    auto f = Sys<int(WINAPI*)()>("D3DPERF_EndEvent");
    return f ? f() : 0;
}
void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n)
{
    if (auto f = Sys<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetMarker"))
        f(c, n);
}
void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n)
{
    if (auto f = Sys<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetRegion"))
        f(c, n);
}
BOOL WINAPI D3DPERF_QueryRepeatFrame()
{
    auto f = Sys<BOOL(WINAPI*)()>("D3DPERF_QueryRepeatFrame");
    return f ? f() : FALSE;
}
DWORD WINAPI D3DPERF_GetStatus()
{
    auto f = Sys<DWORD(WINAPI*)()>("D3DPERF_GetStatus");
    return f ? f() : 0;
}
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
