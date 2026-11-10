// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Legacy mode, game side: a 32-bit d3d9.dll proxy. The game's D3D9(Ex) device runs on D3D9On12 over a D3D12 device
// of ours; at each present the back buffer goes to the x64 NR host through a shared texture and fence and back.
// MultiGpu=1: the 9On12 device is on the render card. Frames cross to the display card through a cross-adapter ring,
// and a presenter thread here does the host round trip there and presents with its own flip swapchain.
// Hotkeys and the menu (Overlay.cpp) write the host's settings into the shared block and the ini.
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <d3d9on12.h>
#include <winternl.h> // NTSTATUS for d3dkmthk.h
#include <d3dkmthk.h>
#include <dxgi1_5.h>
#include <share.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../LegacyShared.h"
#include "Overlay.h"

using Microsoft::WRL::ComPtr;

namespace
{
HMODULE g_self = nullptr;
HMODULE g_sys = nullptr;
std::wstring g_dir, g_ini, g_logPath, g_dumpDir;
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

int IniInt(const wchar_t* key, int def, const wchar_t* sec = L"Legacy")
{
    return GetPrivateProfileIntW(sec, key, def, g_ini.c_str());
}
std::wstring IniStr(const wchar_t* key, const wchar_t* sec = L"Legacy")
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(sec, key, L"", buf, MAX_PATH * 2, g_ini.c_str());
    return buf;
}
float IniFloat(const wchar_t* key, float def, const wchar_t* sec)
{
    const std::wstring s = IniStr(key, sec);
    return s.empty() ? def : (float) _wtof(s.c_str());
}

double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b, const LARGE_INTEGER& f)
{
    return 1000.0 * double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
}

// One device with a direct queue, a copy list ring and an overlay list ring
struct Gpu
{
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> local; // allocator reuse, drains, ReturnUnderlyingResource
    uint64_t value = 0;
    HANDLE ev = nullptr;       // used by one thread only
    ComPtr<ID3D12CommandAllocator> alloc[2];
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t allocValue[2] = {};
    uint32_t next = 0, cur = 0;
    // Overlay: own allocators (3 frames in flight, as the ImGui backend) and an RTV for the target
    ComPtr<ID3D12CommandAllocator> ovAlloc[3];
    ComPtr<ID3D12GraphicsCommandList> ovList;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    uint64_t ovValue[3] = {};
    uint32_t ovNext = 0;
};
Gpu g_r; // the game's 9On12 device: the render card in multi-GPU mode, the only card otherwise
Gpu g_d; // multi-GPU: the display card (presenter and host hand-off)

ComPtr<IDXGIFactory4> g_factory;
ComPtr<ID3D12Fence> g_shared; // with the host, see LegacyShared.h (on the host's card)
HANDLE g_sharedHandle = nullptr, g_event = nullptr;
ComPtr<ID3D12Resource> g_frame;
HANDLE g_frameHandle = nullptr;
uint32_t g_generation = 0;
uint64_t g_n = 0;            // frames signalled to the host
bool g_stalled = false;      // the host missed a deadline: wait for it before the next frame

HANDLE g_mapping = nullptr;
Legacy::Block* g_block = nullptr;
HANDLE g_host = nullptr;
bool g_enabled = true, g_on12 = false, g_mgpu = false;
uint32_t g_timeoutMs = 250;
std::mutex g_mutex;
std::wstring g_bypassPath; // <DumpDir>\proxy_bypass.trigger: while it exists frames skip the host (fps baseline)
std::atomic<bool> g_bypass { false };
bool g_wasSending = true;

bool g_overlayOn = true, g_overlayFailed = false;
DXGI_FORMAT g_overlayFormat = DXGI_FORMAT_UNKNOWN;
std::atomic<bool> g_dumpPending { false };

// Stats (g_statM: the presenter thread writes some of them in multi-GPU mode)
std::mutex g_statM;
std::atomic<uint64_t> g_failedPresents { 0 };
uint64_t g_presents = 0, g_sent = 0, g_returned = 0, g_timeouts = 0, g_skipped = 0, g_bypassed = 0;
LARGE_INTEGER g_freq {}, g_lastStat {};
double g_waitMsSum = 0;
double g_latSum = 0; // game present call -> real present, ms
uint64_t g_latN = 0;
uint32_t g_outW = 0, g_outH = 0;
// Readouts window (500 ms)
LARGE_INTEGER g_lastFps {};
uint64_t g_fpsFrames = 0, g_fpsReturned = 0, g_fpsShown = 0, g_fpsLatN = 0;
double g_fpsWaitSum = 0, g_fpsLatSum = 0;
float g_fps = 0, g_proxyWaitMs = 0, g_shownFps = 0, g_latMs = 0;

// Multi-GPU: ring, fences, presenter
constexpr uint32_t K = 3; // ring slots
struct Ring
{
    ComPtr<ID3D12Heap> heapD, heapR;
    ComPtr<ID3D12Resource> slotD[K], slotR[K];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    D3D12_RESOURCE_DESC desc {}; // the back buffer's
    uint32_t id = 0;
};
struct Meta
{
    std::shared_ptr<Ring> ring;
    LARGE_INTEGER t {}; // the game's present call
    uint64_t frame = 0;
};
using Grave = std::vector<std::pair<std::shared_ptr<Ring>, uint64_t>>; // released once the queue passes the value
ComPtr<ID3D12Fence> g_sendR, g_sendD; // cross-adapter: the render card signals frame m once it is in slot m % K
ComPtr<ID3D12Fence> g_ingest;         // display card: frame n copied out of its slot (the slot may be refilled)
HANDLE g_sendEv = nullptr, g_ingestEv = nullptr;
std::shared_ptr<Ring> g_ring;         // game thread
Grave g_graveR;                       // game thread
uint32_t g_ringIds = 0;
uint32_t g_ahead = 1;                 // frames the game may run ahead of the presenter's ingest (< K)
std::mutex g_pm;
std::condition_variable g_pcv;
Meta g_meta[K];                       // g_pm
uint64_t g_sentN = 0;                 // g_pm
bool g_stop = false;                  // g_pm
std::atomic<bool> g_presenterOn { false }; // the thread is detached: a joinable one at process exit aborts
std::atomic<int> g_mgpuState { 0 };   // 0 not started, 1 presenting, 2 failed or lost: the game's own present
HWND g_presentWnd = nullptr;
// Where each side is, for the logs when one stops
std::atomic<const char*> g_gameStage { "-" }, g_presStage { "-" };
std::atomic<ULONGLONG> g_lastSendTick { 0 };
// Until this tick the game's window proc does not see the messages of our fullscreen switch (the game reacts to them:
// Crysis 2 reset itself to windowed and stopped presenting)
std::atomic<ULONGLONG> g_shieldUntil { 0 };
thread_local bool t_inCreate = false; // inside Direct3DCreate9On12(Ex) / CreateDevice(Ex), see HookAdapterLuid
UINT g_sync = 0;                      // from the game's presentation interval
bool g_wantFs = false;                // the game asked for exclusive fullscreen (9On12 runs it windowed)
int g_fsMode = 1; // MgpuFullscreen: 0 window as the game made it, 1 window over the monitor (scaled), 2 + display mode, 3 DXGI
bool g_modeSet = false;
UINT g_fsW = 0, g_fsH = 0, g_fsHz = 0;
// presenter thread
ComPtr<IDXGISwapChain3> g_sc;
constexpr UINT kScBuffers = 3;
ComPtr<ID3D12Resource> g_scBuf[kScBuffers];
UINT g_scW = 0, g_scH = 0, g_scFlags = 0;
DXGI_FORMAT g_scFmt = DXGI_FORMAT_UNKNOWN;
bool g_tearing = false, g_occluded = false, g_scFs = false;
// multi-GPU stats (g_statM)
uint64_t g_shown = 0, g_ingested = 0, g_occludedFrames = 0, g_sendStalls = 0, g_dropped = 0;
double g_slotWaitSum = 0, g_sendWaitSum = 0;
uint64_t g_slotWaitN = 0, g_sendWaitN = 0;

// Settings: the menu / hotkeys own them, the host applies them from the block
std::mutex g_setMutex; // never held while calling into Overlay
Overlay::Settings g_set;
std::atomic<bool> g_nrOn { true };
std::atomic<ULONGLONG> g_iniDue { 0 }; // save the ini at this tick (0: nothing pending)

// Hotkeys (VK codes, [Legacy] Key* in the ini): unbound in HL2 by default
enum Action
{
    kMenu,
    kNr,
    kSize,
    kHistory,
    kFlow,
    kActions
};
UINT g_keys[kActions] = { VK_INSERT, VK_END, VK_PRIOR, VK_NEXT, VK_HOME };
const wchar_t* const kKeyNames[kActions] = { L"KeyMenu", L"KeyNr", L"KeySize", L"KeyHistory", L"KeyFlow" };
const char* const kActionNames[kActions] = { "menu", "nr", "size", "history", "flow" };
bool g_hotkeys = true;
HWND g_wnd = nullptr;
WNDPROC g_origProc = nullptr;
bool g_swallowEscUp = false;

const char* const kSizeNames[] = { "1080 tier", "1440", "native" };
const char* const kFlowNames[] = { "off (no motion: history off)", "off (static history)", "on" };

void LoadSettings()
{
    Overlay::Settings s;
    s.nr = IniInt(L"Nr", 1) != 0;
    auto& c = s.c;
    c.model = std::clamp(IniInt(L"Model", 1, L"Host"), 0, 1);
    c.size = std::clamp(IniInt(L"NetworkSize", 0, L"Host"), 0, 2);
    c.passes = std::clamp(IniInt(L"MultiPass", 1, L"Host"), 1, 3);
    c.style = std::clamp(IniInt(L"Style", 0, L"Host"), -1, 2);
    c.temporal = IniInt(L"Temporal", 1, L"Host") != 0;
    c.flow = std::clamp(IniInt(L"OpticalFlow", 2, L"Host"), 0, 2);
    c.upsample = std::clamp(IniInt(L"EditUpsample", 1, L"Host"), 0, 1);
    c.stabilise = IniInt(L"EditStabilise", 1, L"Host") != 0;
    c.strength = std::clamp(IniFloat(L"Strength", 1.0f, L"Host"), 0.0f, 2.0f);
    c.colorStrength = std::clamp(IniFloat(L"ColorStrength", 1.0f, L"Host"), -1.0f, 1.0f);
    std::lock_guard lock(g_setMutex);
    g_set = s;
    g_nrOn = s.nr;
}

// Block write: fields first, then the serial the host watches
void PushControl(const Legacy::Control& c)
{
    if (g_block == nullptr)
        return;
    const uint32_t serial = g_block->ctl.serial + 1;
    Legacy::Control v = c;
    v.serial = g_block->ctl.serial;
    g_block->ctl = v;
    MemoryBarrier();
    g_block->ctl.serial = serial;
}

void SaveIni()
{
    Overlay::Settings s;
    {
        std::lock_guard lock(g_setMutex);
        s = g_set;
    }
    auto put = [](const wchar_t* sec, const wchar_t* k, const std::wstring& v)
    { WritePrivateProfileStringW(sec, k, v.c_str(), g_ini.c_str()); };
    auto f2 = [](float v)
    {
        wchar_t b[32];
        swprintf_s(b, L"%.2f", v);
        return std::wstring(b);
    };
    const auto& c = s.c;
    put(L"Legacy", L"Nr", s.nr ? L"1" : L"0");
    put(L"Host", L"Model", std::to_wstring(c.model));
    put(L"Host", L"NetworkSize", std::to_wstring(c.size));
    put(L"Host", L"MultiPass", std::to_wstring(c.passes));
    put(L"Host", L"Style", std::to_wstring(c.style));
    put(L"Host", L"Temporal", std::to_wstring(c.temporal));
    put(L"Host", L"OpticalFlow", std::to_wstring(c.flow));
    put(L"Host", L"EditUpsample", std::to_wstring(c.upsample));
    put(L"Host", L"EditStabilise", std::to_wstring(c.stabilise));
    put(L"Host", L"Strength", f2(c.strength));
    put(L"Host", L"ColorStrength", f2(c.colorStrength));
    Log("settings saved: nr %d model %d size %d passes %d style %d temporal %d flow %d upsample %d stabilise %d "
        "strength %.2f colour %.2f", s.nr, c.model, c.size, c.passes, c.style, c.temporal, c.flow, c.upsample,
        c.stabilise, c.strength, c.colorStrength);
}

void DoAction(int a)
{
    if (a == kMenu)
    {
        const bool open = !Overlay::MenuOpen();
        Overlay::SetMenu(open);
        Log("hotkey: menu %s", open ? "open" : "closed");
        return;
    }
    Overlay::Settings s = Overlay::GetSettings();
    char msg[160] = {};
    auto& c = s.c;
    const uint32_t caps = g_block ? g_block->caps : 0;
    const bool hostKnown = g_block && g_block->hostReady;
    switch (a)
    {
    case kNr:
        s.nr = !s.nr;
        snprintf(msg, sizeof(msg), "NR %s", s.nr ? "on" : "off (bypass)");
        break;
    case kSize:
        if (hostKnown && !(caps & Legacy::kCapFreeRes))
        {
            c.size = 0;
            snprintf(msg, sizeof(msg), "Network size: 1080 tier only in this runtime");
        }
        else
        {
            c.size = (c.size + 1) % 3;
            snprintf(msg, sizeof(msg), "Network size: %s (rebuilding, a few seconds without NR)", kSizeNames[c.size]);
        }
        break;
    case kHistory:
        c.temporal = !c.temporal;
        snprintf(msg, sizeof(msg), "Network history %s", c.temporal ? "on" : "off");
        break;
    case kFlow:
        c.flow = c.flow == 2 ? 1 : 2;
        snprintf(msg, sizeof(msg), "Optical flow %s", kFlowNames[c.flow]);
        if (!c.temporal && !(c.upsample == 1 && c.stabilise))
            strncat_s(msg, " - history is off, so it has no effect", _TRUNCATE);
        break;
    default:
        return;
    }
    Overlay::SetSettings(s);
    Overlay::Toast("%s", msg);
    Log("hotkey %s: %s", kActionNames[a], msg);
}

bool ShieldedMessage(UINT m)
{
    switch (m)
    {
    case WM_SIZE:
    case WM_MOVE:
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
    case WM_DISPLAYCHANGE:
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_NCACTIVATE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SIZING:
    case WM_STYLECHANGING:
    case WM_STYLECHANGED: return true;
    default: return false;
    }
}

LRESULT CALLBACK SubProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (ShieldedMessage(m) && GetTickCount64() < g_shieldUntil)
    {
        static int logged = 0;
        if (logged++ < 20)
            Log("fullscreen switch: message %04x kept from the game", m);
        return DefWindowProcW(w, m, wp, lp);
    }
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP)
    {
        const bool down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN;
        if (g_hotkeys)
            for (int a = 0; a < kActions; ++a)
                if (g_keys[a] != 0 && wp == g_keys[a])
                {
                    if (down && !(lp & (1 << 30))) // first press, not auto-repeat
                        DoAction(a);
                    return 0; // the game never sees our keys
                }
        if (Overlay::MenuOpen() && wp == VK_ESCAPE && down)
        {
            Overlay::SetMenu(false);
            g_swallowEscUp = true;
            Log("menu closed (Esc)");
            return 0;
        }
        if (g_swallowEscUp && wp == VK_ESCAPE && !down)
        {
            g_swallowEscUp = false;
            return 0;
        }
    }
    if (Overlay::OnMessage(w, m, wp, lp))
        return m == WM_INPUT ? DefWindowProcW(w, m, wp, lp) : 0; // WM_INPUT: the system's cleanup
    return CallWindowProcW(g_origProc, w, m, wp, lp);
}

void Subclass(HWND w)
{
    if (w == nullptr || g_wnd != nullptr || (!g_hotkeys && !g_mgpu)) // multi-GPU: the fullscreen shield
        return;
    g_wnd = w;
    g_origProc = (WNDPROC) SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR) &SubProc);
    Log("window %p subclassed (%s), keys: menu %u, NR %u, size %u, history %u, flow %u", w,
        g_origProc ? "ok" : "FAILED", g_keys[kMenu], g_keys[kNr], g_keys[kSize], g_keys[kHistory], g_keys[kFlow]);
    if (g_origProc == nullptr)
        g_wnd = nullptr;
}

// Waits until f reaches v. Re-checks the value: a registration that fired after an earlier wait gave up leaves the
// auto-reset event set, and a bare WaitForSingleObject would take that as this frame's answer.
bool WaitValue(ID3D12Fence* f, uint64_t v, HANDLE e, DWORD ms)
{
    const ULONGLONG end = GetTickCount64() + ms;
    while (f->GetCompletedValue() < v)
    {
        const ULONGLONG now = GetTickCount64();
        if (now >= end || FAILED(f->SetEventOnCompletion(v, e)))
            return false;
        WaitForSingleObject(e, DWORD(end - now));
    }
    return true;
}

bool CreateGpu(Gpu& g, IDXGIAdapter1* a)
{
    if (FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev))))
        return false;
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue))))
        return false;
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.local));
    for (auto& x : g.alloc)
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&x));
    g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0].Get(), nullptr, IID_PPV_ARGS(&g.list));
    if (!g.local || !g.list)
        return false;
    g.list->Close();
    g.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

ID3D12GraphicsCommandList* Begin(Gpu& g)
{
    g.cur = g.next++ & 1;
    WaitValue(g.local.Get(), g.allocValue[g.cur], g.ev, 1000);
    g.alloc[g.cur]->Reset();
    g.list->Reset(g.alloc[g.cur].Get(), nullptr);
    return g.list.Get();
}

void Submit(Gpu& g)
{
    g.list->Close();
    ID3D12CommandList* l[] = { g.list.Get() };
    g.queue->ExecuteCommandLists(1, l);
    g.allocValue[g.cur] = ++g.value;
    g.queue->Signal(g.local.Get(), g.value);
}

bool Drain(Gpu& g, DWORD ms = 1000)
{
    g.queue->Signal(g.local.Get(), ++g.value);
    return WaitValue(g.local.Get(), g.value, g.ev, ms);
}

void Prune(Grave& grave, Gpu& g)
{
    const uint64_t done = g.local->GetCompletedValue();
    std::erase_if(grave, [&](const auto& e) { return e.second <= done; });
}

Gpu& HostGpu() { return g_mgpu ? g_d : g_r; }

bool SameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

int PciBus(LUID luid)
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

// The render card: MgpuRenderBus (PCI bus), else the hardware adapter on a PCI bus without outputs with the most memory
ComPtr<IDXGIAdapter1> RenderAdapter(const LUID& display)
{
    const int want = IniInt(L"MgpuRenderBus", -1);
    ComPtr<IDXGIAdapter1> best;
    SIZE_T bestMem = 0;
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> a;
        if (g_factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 d {};
        a->GetDesc1(&d);
        ComPtr<IDXGIOutput> o;
        const bool outputs = SUCCEEDED(a->EnumOutputs(0, &o));
        const int bus = PciBus(d.AdapterLuid);
        Log("adapter %u: %ls, bus %d, LUID %08x:%08x, %u MB, outputs %d, flags %x", i, d.Description, bus,
            d.AdapterLuid.HighPart, d.AdapterLuid.LowPart, unsigned(d.DedicatedVideoMemory >> 20), outputs, d.Flags);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || SameLuid(d.AdapterLuid, display) || bus < 0)
            continue;
        if (want >= 0 ? bus == want : !outputs && (!best || d.DedicatedVideoMemory > bestMem))
            best = a, bestMem = d.DedicatedVideoMemory;
    }
    return best;
}

bool InitMgpu()
{
    HANDLE h = nullptr;
    if (FAILED(g_r.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                                    IID_PPV_ARGS(&g_sendR))) ||
        FAILED(g_r.dev->CreateSharedHandle(g_sendR.Get(), nullptr, GENERIC_ALL, nullptr, &h)))
        return false;
    const HRESULT hr = g_d.dev->OpenSharedHandle(h, IID_PPV_ARGS(&g_sendD));
    CloseHandle(h);
    if (FAILED(hr) || FAILED(g_d.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_ingest))))
        return false;
    g_sendEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_ingestEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

bool InitD3D12()
{
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g_factory))))
        return false;
    ComPtr<IDXGIAdapter1> display;
    const int want = IniInt(L"Adapter", 0); // DXGI index; the game's D3D9 default adapter is the primary one
    if (FAILED(g_factory->EnumAdapters1(want, &display)))
        return false;
    DXGI_ADAPTER_DESC1 dd {};
    display->GetDesc1(&dd);
    if (g_mgpu)
    {
        ComPtr<IDXGIAdapter1> render = RenderAdapter(dd.AdapterLuid);
        DXGI_ADAPTER_DESC1 rd {};
        if (render)
            render->GetDesc1(&rd);
        if (render && CreateGpu(g_d, display.Get()) && CreateGpu(g_r, render.Get()) && InitMgpu())
            Log("multi-GPU: render %ls (LUID %08x:%08x), display %ls (LUID %08x:%08x), ring %u slots, ahead %u",
                rd.Description, rd.AdapterLuid.HighPart, rd.AdapterLuid.LowPart, dd.Description,
                dd.AdapterLuid.HighPart, dd.AdapterLuid.LowPart, K, g_ahead);
        else
        {
            Log("multi-GPU: %s: single GPU on %ls", render ? "set-up failed" : "no render card found", dd.Description);
            g_mgpu = false;
            g_r = Gpu {}, g_d = Gpu {};
            g_sendR.Reset(), g_sendD.Reset(), g_ingest.Reset();
        }
    }
    if (!g_mgpu && !CreateGpu(g_r, display.Get()))
    {
        Log("D3D12CreateDevice failed on %ls", dd.Description);
        return false;
    }
    Gpu& h = HostGpu();
    h.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_shared));
    if (!g_shared)
        return false;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    const DWORD pid = GetCurrentProcessId();
    wchar_t name[128];
    swprintf_s(name, Legacy::kFenceName, pid);
    if (FAILED(h.dev->CreateSharedHandle(g_shared.Get(), nullptr, GENERIC_ALL, name, &g_sharedHandle)))
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
    const LUID luid = h.dev->GetAdapterLuid(); // the host runs on this card
    g_block->adapterLuidLow = luid.LowPart;
    g_block->adapterLuidHigh = luid.HighPart;
    {
        std::lock_guard lock(g_setMutex);
        PushControl(g_set.c);
    }
    const LUID rl = g_r.dev->GetAdapterLuid();
    Log("D3D12 device for 9On12 on LUID %08x:%08x, host card LUID %08x:%08x (%ls)", rl.HighPart, rl.LowPart,
        luid.HighPart, luid.LowPart, dd.Description);
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

// The texture the host works on, on its card (g)
bool EnsureFrame(Gpu& g, const D3D12_RESOURCE_DESC& bb)
{
    if (g_frame)
    {
        const auto d = g_frame->GetDesc();
        if (d.Width == bb.Width && d.Height == bb.Height && d.Format == bb.Format)
            return true;
        Drain(g); // our queue's last copies of the old one; the host holds its own reference
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
    HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&g_frame));
    if (FAILED(hr))
    {
        Log("frame texture %llux%u format %u failed: %08x", d.Width, d.Height, d.Format, hr);
        return false;
    }
    ++g_generation;
    wchar_t name[128];
    swprintf_s(name, Legacy::kFrameName, GetCurrentProcessId(), g_generation);
    hr = g.dev->CreateSharedHandle(g_frame.Get(), nullptr, GENERIC_ALL, name, &g_frameHandle);
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

// Copies src -> dst on g's queue (both COMMON at the boundaries)
void Copy(Gpu& g, ID3D12Resource* dst, ID3D12Resource* src)
{
    auto* l = Begin(g);
    Barrier(l, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(l, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyResource(dst, src);
    Barrier(l, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(l, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Submit(g);
}

DXGI_FORMAT RtvFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

// Menu / toast onto the target (COMMON before and after) on g's queue
void DrawOverlay(Gpu& g, ID3D12Resource* bb, const D3D12_RESOURCE_DESC& desc)
{
    if (g_overlayFailed)
        return;
    const DXGI_FORMAT fmt = RtvFormat(desc.Format);
    if (g_overlayFormat != fmt)
    {
        if (g_overlayFormat != DXGI_FORMAT_UNKNOWN)
        {
            Drain(g);
            Overlay::Shutdown();
        }
        if (!g.ovList)
        {
            for (auto& a : g.ovAlloc)
                g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
            g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.ovAlloc[0].Get(), nullptr,
                                     IID_PPV_ARGS(&g.ovList));
            D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
            g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap));
            if (g.ovList)
                g.ovList->Close();
        }
        if (!g.ovList || !g.rtvHeap || !Overlay::Init(g.dev.Get(), g.queue.Get(), fmt, desc.Height))
        {
            g_overlayFailed = true;
            Log("overlay init failed (format %u): toasts and menu off, hotkeys still work", fmt);
            return;
        }
        g_overlayFormat = fmt;
        Log("overlay ready (format %u, %llux%u)", fmt, desc.Width, desc.Height);
    }
    const uint32_t i = g.ovNext++ % 3;
    WaitValue(g.local.Get(), g.ovValue[i], g.ev, 1000);
    g.ovAlloc[i]->Reset();
    g.ovList->Reset(g.ovAlloc[i].Get(), nullptr);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rd {};
    rd.Format = fmt;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.dev->CreateRenderTargetView(bb, &rd, rtv);
    Barrier(g.ovList.Get(), bb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Overlay::Draw(g.ovList.Get(), bb, rtv, (uint32_t) desc.Width, desc.Height);
    Barrier(g.ovList.Get(), bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    g.ovList->Close();
    ID3D12CommandList* l[] = { g.ovList.Get() };
    g.queue->ExecuteCommandLists(1, l);
    g.ovValue[i] = ++g.value;
    g.queue->Signal(g.local.Get(), g.value);
}

// <DumpDir>\proxy_dump.trigger: the back buffer as presented (overlay included) to <DumpDir>\proxy_frame.bmp
void DumpBackBuffer(Gpu& g, ID3D12Resource* bb, const D3D12_RESOURCE_DESC& desc)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    g.dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&rb))))
        return;
    auto* l = Begin(g);
    Barrier(l, bb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst { rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src { bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(l, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Submit(g);
    if (!WaitValue(g.local.Get(), g.value, g.ev, 2000))
        return;
    const uint32_t w = (uint32_t) desc.Width, h = desc.Height;
    uint8_t* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, (void**) &p)))
        return;
    const std::wstring path = g_dumpDir + L"\\proxy_frame.bmp";
    if (FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYNO))
    {
        BITMAPFILEHEADER fh { 0x4D42 };
        BITMAPINFOHEADER ih { sizeof(ih), LONG(w), -LONG(h), 1, 24 };
        const uint32_t row = (w * 3 + 3) & ~3u;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + row * h;
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        std::string line(row, '\0');
        const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                          desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t* s = p + fp.Offset + UINT64(y) * fp.Footprint.RowPitch;
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* q = s + x * 4;
                line[x * 3 + 0] = char(bgra ? q[0] : q[2]);
                line[x * 3 + 1] = char(q[1]);
                line[x * 3 + 2] = char(bgra ? q[2] : q[0]);
            }
            fwrite(line.data(), 1, row, f);
        }
        fclose(f);
        Log("back buffer dumped: %ls (%ux%u format %u, menu %d)", path.c_str(), w, h, desc.Format,
            Overlay::MenuOpen());
    }
    rb->Unmap(0, nullptr);
}

void Stats()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::lock_guard sl(g_statM);
    if (g_lastStat.QuadPart == 0)
        g_lastStat = g_lastFps = now;
    const double fs = double(now.QuadPart - g_lastFps.QuadPart) / double(g_freq.QuadPart);
    if (fs >= 0.5)
    {
        const uint64_t ret = g_returned - g_fpsReturned, lat = g_latN - g_fpsLatN;
        g_fps = float(double(g_presents - g_fpsFrames) / fs);
        g_shownFps = float(double(g_shown - g_fpsShown) / fs);
        g_proxyWaitMs = ret ? float(g_fpsWaitSum / double(ret)) : 0.0f;
        g_latMs = lat ? float(g_fpsLatSum / double(lat)) : 0.0f;
        g_fpsFrames = g_presents, g_fpsReturned = g_returned, g_fpsWaitSum = 0, g_lastFps = now;
        g_fpsShown = g_shown, g_fpsLatN = g_latN, g_fpsLatSum = 0;
    }
    const double s = double(now.QuadPart - g_lastStat.QuadPart) / double(g_freq.QuadPart);
    if (s < 4.0)
        return;
    const uint64_t frames = g_presents;
    static uint64_t lastFrames = 0, lastReturned = 0, lastShown = 0, lastLatN = 0;
    static double lastLatSum = 0;
    const double fps = double(frames - lastFrames) / s;
    const uint64_t ret = g_returned - lastReturned;
    const uint64_t latN = g_latN - lastLatN;
    const double latMs = latN ? (g_latSum - lastLatSum) / double(latN) : 0.0;
    Log("pacing: %.1f fps, %llu presents, %llu returned from the host (%llu in window), %llu timeouts, %llu skipped, "
        "%llu failed presents, host wait %.2f ms/frame, host %s state %d gpu %.2f ms, of %.2f ms, flk %.5f / %.5f, %s, "
        "%llu bypassed, net %.2f ms, latency %.2f ms",
        fps, frames, g_returned, ret, g_timeouts, g_skipped, g_failedPresents.load(), ret ? g_waitMsSum / double(ret) : 0.0,
        HostAlive() ? "up" : "down", g_block ? g_block->hostState : -1, g_block ? g_block->hostMs : 0.f,
        g_block ? g_block->ofMs : 0.f, g_block ? g_block->flkStatic : 0.f, g_block ? g_block->flkMc : 0.f,
        g_block ? g_block->status : "", g_bypassed, g_block ? g_block->netMs : 0.f, latMs);
    if (g_r.dev && g_shared)
        Log("  device %08x, shared fence %llu, frame %llu", (unsigned) g_r.dev->GetDeviceRemovedReason(),
            g_shared->GetCompletedValue(), g_n);
    if (g_mgpu)
    {
        Log("  mgpu %s: %.1f fps shown (%llu presents), %llu ingested, %llu dropped, %llu occluded, slot wait %.2f "
            "ms/frame, send wait %.2f ms, %llu send stalls, send fence %llu, ingest %llu",
            g_mgpuState == 1 ? "presenting" : g_mgpuState == 2 ? "OFF (failed or lost)" : "starting",
            double(g_shown - lastShown) / s, g_shown, g_ingested, g_dropped, g_occludedFrames,
            g_slotWaitN ? g_slotWaitSum / double(g_slotWaitN) : 0.0, g_sendWaitN ? g_sendWaitSum / double(g_sendWaitN) : 0.0,
            g_sendStalls, g_sendR ? g_sendR->GetCompletedValue() : 0, g_ingest ? g_ingest->GetCompletedValue() : 0);
        Log("  display device %08x, presenter at: %s", g_d.dev ? (unsigned) g_d.dev->GetDeviceRemovedReason() : 0u,
            g_presStage.load());
        g_slotWaitSum = g_sendWaitSum = 0, g_slotWaitN = g_sendWaitN = 0;
    }
    lastFrames = frames;
    lastReturned = g_returned;
    lastShown = g_shown;
    lastLatN = g_latN, lastLatSum = g_latSum;
    g_waitMsSum = 0;
    g_lastStat = now;
}

// Trigger files in the dump folder, every 30 presents: bypass (exists = on), keys (one action name per line, as
// the hotkeys), dump
void PollFiles()
{
    if (g_dumpDir.empty() || g_presents % 30 != 0)
        return;
    const bool b = GetFileAttributesW(g_bypassPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (b != g_bypass)
        Log("bypass %s", b ? "on" : "off");
    g_bypass = b;
    const std::wstring keys = g_dumpDir + L"\\proxy_keys.trigger";
    if (GetFileAttributesW(keys.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        std::ifstream f(keys);
        std::string line;
        while (std::getline(f, line))
            for (int a = 0; a < kActions; ++a)
                if (line == kActionNames[a])
                    DoAction(a);
        f.close();
        DeleteFileW(keys.c_str());
    }
    const std::wstring dump = g_dumpDir + L"\\proxy_dump.trigger";
    if (GetFileAttributesW(dump.c_str()) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW(dump.c_str()), g_dumpPending = true;
}

// Which frames go to the host (presenting thread). false: straight to the screen.
bool RouteToHost()
{
    bool send = g_enabled && g_nrOn && !g_bypass && HostAlive();
    if (send && !g_wasSending)
        g_block->reset = 1; // history break after a bypass
    g_wasSending = send;
    if (!send && g_enabled && HostAlive())
        ++g_bypassed;
    if (send && g_stalled)
    {
        if (g_shared->GetCompletedValue() < 2 * g_n)
            ++g_skipped, send = false;
        else
            g_stalled = false;
    }
    return send;
}

// Signals frame n to the host and waits on the CPU for its answer (the frame texture already holds n)
bool HostRoundTrip()
{
    ++g_n;
    g_block->frame = g_n;
    MemoryBarrier();
    HostGpu().queue->Signal(g_shared.Get(), 2 * g_n - 1);
    ++g_sent;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const bool back = WaitValue(g_shared.Get(), 2 * g_n, g_event, g_timeoutMs);
    QueryPerformanceCounter(&t1);
    std::lock_guard sl(g_statM);
    if (back)
    {
        ++g_returned;
        const double ms = Ms(t0, t1, g_freq);
        g_waitMsSum += ms;
        g_fpsWaitSum += ms;
    }
    else
    {
        g_stalled = true;
        if (g_timeouts++ < 10)
            Log("frame %llu: host did not answer within %u ms", g_n, g_timeoutMs);
    }
    return back;
}

// ---- Multi-GPU ----

DXGI_FORMAT ScFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// K row-major slots in one cross-adapter heap (system memory), placed on both cards
std::shared_ptr<Ring> MakeRing(const D3D12_RESOURCE_DESC& bb)
{
    auto r = std::make_shared<Ring>();
    r->desc = bb;
    r->id = ++g_ringIds;
    UINT64 total = 0;
    g_r.dev->GetCopyableFootprints(&bb, 0, 1, 0, &r->fp, nullptr, nullptr, &total);
    const UINT64 slot = (total + 65535) & ~UINT64(65535);
    D3D12_HEAP_DESC hd {};
    hd.SizeInBytes = slot * K;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    HRESULT hr = g_d.dev->CreateHeap(&hd, IID_PPV_ARGS(&r->heapD));
    HANDLE h = nullptr;
    if (SUCCEEDED(hr))
        hr = g_d.dev->CreateSharedHandle(r->heapD.Get(), nullptr, GENERIC_ALL, nullptr, &h);
    if (SUCCEEDED(hr))
    {
        hr = g_r.dev->OpenSharedHandle(h, IID_PPV_ARGS(&r->heapR));
        CloseHandle(h);
    }
    D3D12_RESOURCE_DESC b {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = slot;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    b.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    for (UINT s = 0; s < K && SUCCEEDED(hr); ++s)
    {
        hr = g_d.dev->CreatePlacedResource(r->heapD.Get(), s * slot, &b, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                           IID_PPV_ARGS(&r->slotD[s]));
        if (SUCCEEDED(hr))
            hr = g_r.dev->CreatePlacedResource(r->heapR.Get(), s * slot, &b, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&r->slotR[s]));
    }
    if (FAILED(hr))
    {
        Log("multi-GPU ring %llux%u format %u failed: %08x", bb.Width, bb.Height, bb.Format, hr);
        return nullptr;
    }
    Log("multi-GPU ring %u: %llux%u format %u, %u slots of %llu KB (row pitch %u)", r->id, bb.Width, bb.Height,
        bb.Format, K, slot >> 10, r->fp.Footprint.RowPitch);
    return r;
}

void CopyFromSlot(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, const Ring& r, UINT s)
{
    D3D12_TEXTURE_COPY_LOCATION d { dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION src { r.slotD[s].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    src.PlacedFootprint = r.fp;
    l->CopyTextureRegion(&d, 0, 0, 0, &src, nullptr);
}

bool GetScBuffers()
{
    for (UINT i = 0; i < kScBuffers; ++i)
        if (FAILED(g_sc->GetBuffer(i, IID_PPV_ARGS(&g_scBuf[i]))))
            return false;
    return true;
}

// MgpuFullscreen=3: DXGI exclusive fullscreen on the display card at the game's mode. Crysis 2 pauses after it (lost device)
void EnterFullscreen()
{
    g_shieldUntil = GetTickCount64() + 60000;
    DXGI_MODE_DESC md {};
    md.Width = g_fsW, md.Height = g_fsH, md.Format = g_scFmt;
    md.RefreshRate = { g_fsHz, g_fsHz ? 1u : 0u };
    HRESULT hr = g_sc->ResizeTarget(&md);
    const HRESULT fs = g_sc->SetFullscreenState(TRUE, nullptr);
    BOOL now = FALSE;
    g_sc->GetFullscreenState(&now, nullptr);
    g_scFs = now != FALSE;
    if (g_scFs)
    {
        Drain(g_d);
        for (auto& b : g_scBuf)
            b.Reset();
        hr = g_sc->ResizeBuffers(kScBuffers, g_scW, g_scH, g_scFmt, g_scFlags);
        if (FAILED(hr) || !GetScBuffers())
            Log("fullscreen: ResizeBuffers failed %08x", hr);
    }
    g_shieldUntil = GetTickCount64() + 750; // posted messages arrive after the call
    static int logged = 0;
    if (logged++ < 10)
        Log("fullscreen %ux%u@%u: ResizeTarget %08x, SetFullscreenState %08x, now %d", g_fsW, g_fsH, g_fsHz, hr, fs,
            g_scFs);
}

bool EnsureSwapchain(const D3D12_RESOURCE_DESC& d)
{
    const DXGI_FORMAT fmt = ScFormat(d.Format);
    if (fmt == DXGI_FORMAT_UNKNOWN)
    {
        Log("multi-GPU: back buffer format %u has no flip swapchain format", d.Format);
        return false;
    }
    if (g_sc && g_scW == d.Width && g_scH == d.Height && g_scFmt == fmt)
        return true;
    HRESULT hr;
    if (!g_sc)
    {
        ComPtr<IDXGIFactory5> f5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(g_factory.As(&f5)))
            f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        g_tearing = tearing != FALSE;
        g_scFlags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        if (g_wantFs && g_fsMode == 3)
            g_scFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        DXGI_SWAP_CHAIN_DESC1 sd {};
        sd.Width = (UINT) d.Width;
        sd.Height = d.Height;
        sd.Format = fmt;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kScBuffers;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = g_scFlags;
        ComPtr<IDXGISwapChain1> sc1;
        hr = g_factory->CreateSwapChainForHwnd(g_d.queue.Get(), g_presentWnd, &sd, nullptr, nullptr, &sc1);
        if (FAILED(hr) || FAILED(sc1.As(&g_sc)))
        {
            Log("multi-GPU: swapchain for window %p failed: %08x", g_presentWnd, hr);
            return false;
        }
        g_factory->MakeWindowAssociation(g_presentWnd, DXGI_MWA_NO_ALT_ENTER);
        g_scW = sd.Width, g_scH = sd.Height, g_scFmt = fmt;
        if (!GetScBuffers())
            return false;
        RECT wr {};
        GetWindowRect(g_presentWnd, &wr);
        Log("multi-GPU: flip swapchain %ux%u format %u on the display card, window %p %ldx%ld, tearing %d, sync %u, "
            "fullscreen wanted %d", sd.Width, sd.Height, fmt, g_presentWnd, wr.right - wr.left, wr.bottom - wr.top,
            g_tearing, g_sync, g_wantFs);
        Log("fullscreen mode %d, window in front %d, display mode set %d", g_fsMode,
            GetForegroundWindow() == g_presentWnd, g_modeSet);
        if (g_wantFs && g_fsMode == 3)
            EnterFullscreen();
        return true;
    }
    // Resize: nothing of ours may still use the old buffers
    Drain(g_d);
    for (auto& b : g_scBuf)
        b.Reset();
    hr = g_sc->ResizeBuffers(kScBuffers, (UINT) d.Width, d.Height, fmt, g_scFlags);
    if (FAILED(hr) || !GetScBuffers())
    {
        Log("multi-GPU: ResizeBuffers %llux%u failed: %08x", d.Width, d.Height, hr);
        return false;
    }
    g_scW = (UINT) d.Width, g_scH = d.Height, g_scFmt = fmt;
    Log("multi-GPU: swapchain resized to %ux%u format %u", g_scW, g_scH, fmt);
    return true;
}

bool DeviceLost(const char* where)
{
    const HRESULT d = g_d.dev->GetDeviceRemovedReason(), r = g_r.dev->GetDeviceRemovedReason();
    if (SUCCEEDED(d) && SUCCEEDED(r))
        return false;
    Log("multi-GPU stopped at %s: display device %08x, render device %08x", where, (unsigned) d, (unsigned) r);
    g_mgpuState = 2;
    return true;
}

// Frame n: ingest out of its slot, the host round trip (or straight through), overlay, present
void PresentFrame(uint64_t n, const Meta& m)
{
    const Ring& ring = *m.ring;
    const UINT s = UINT(n % K);
    g_presStage = "swapchain";
    if (!EnsureSwapchain(ring.desc))
    {
        g_mgpuState = 2;
        return;
    }
    g_mgpuState = 1;
    const UINT bi = g_sc->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = g_scBuf[bi].Get();
    const auto bbDesc = bb->GetDesc();
    bool nr = false, ingested = false;
    if (RouteToHost() && EnsureFrame(g_d, ring.desc))
    {
        auto* l = Begin(g_d);
        Barrier(l, g_frame.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        CopyFromSlot(l, g_frame.Get(), ring, s);
        Barrier(l, g_frame.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Submit(g_d);
        g_d.queue->Signal(g_ingest.Get(), n); // the game may refill the slot while the host works
        ingested = true;
        g_presStage = "host answer";
        nr = HostRoundTrip();
    }
    auto* l = Begin(g_d);
    Barrier(l, bb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    if (nr)
    {
        Barrier(l, g_frame.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyResource(bb, g_frame.Get());
        Barrier(l, g_frame.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    else
        CopyFromSlot(l, bb, ring, s); // slot n is refilled only after ingest n + 1 (g_ahead < K)
    Barrier(l, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Submit(g_d);
    if (!ingested)
        g_d.queue->Signal(g_ingest.Get(), n);
    if (g_overlayOn && Overlay::Visible())
    {
        g_presStage = "overlay";
        DrawOverlay(g_d, bb, bbDesc);
    }
    if (g_dumpPending.exchange(false))
        DumpBackBuffer(g_d, bb, bbDesc);
    {
        std::lock_guard sl(g_statM);
        ++g_ingested;
    }

    // Nothing queued on the display card waits on anything external: the Present goes behind copies and draws only
    if (g_occluded || IsIconic(g_presentWnd))
    {
        g_occluded = true;
        if (IsIconic(g_presentWnd) || g_sc->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            std::lock_guard sl(g_statM);
            ++g_occludedFrames;
            return;
        }
        g_occluded = false;
        Log("window shows again at frame %llu", n);
    }
    UINT flags = g_sync == 0 && g_tearing && !g_scFs ? DXGI_PRESENT_ALLOW_TEARING : 0;
    g_presStage = "Present";
    HRESULT hr = g_sc->Present(g_sync, flags);
    if (hr == DXGI_ERROR_INVALID_CALL && flags)
        hr = g_sc->Present(g_sync, 0);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (hr == DXGI_STATUS_OCCLUDED)
    {
        g_occluded = true;
        Log("window minimised or occluded at frame %llu: presents held until it shows", n);
    }
    else if (FAILED(hr))
    {
        if (g_failedPresents++ < 10)
            Log("swapchain Present failed: %08x", hr);
        DeviceLost("Present");
    }
    else
    {
        std::lock_guard sl(g_statM);
        ++g_shown;
        const double ms = Ms(m.t, t, g_freq);
        g_latSum += ms, ++g_latN, g_fpsLatSum += ms;
    }
}

// Every 500 ms: fullscreen back after an Alt+Tab, device check
void PresenterIdle()
{
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 500)
        return;
    last = now;
    if (!g_sc || !g_wantFs || g_fsMode == 0)
        return;
    if (g_fsMode != 3)
        return;
    BOOL fs = FALSE;
    g_sc->GetFullscreenState(&fs, nullptr);
    if (g_scFs && !fs)
        Log("fullscreen left (focus lost)");
    g_scFs = fs != FALSE;
    if (!g_scFs && GetForegroundWindow() == g_presentWnd && !IsIconic(g_presentWnd))
        EnterFullscreen();
}

void PresenterLoop()
{
    SetThreadDescription(GetCurrentThread(), L"lmxxf legacy presenter");
    uint64_t last = 0;
    std::shared_ptr<Ring> held;
    Grave grave;
    LARGE_INTEGER t0, t1;
    while (g_mgpuState != 2)
    {
        uint64_t target = 0;
        {
            std::unique_lock lk(g_pm);
            g_presStage = "waiting for a frame";
            g_pcv.wait_for(lk, std::chrono::milliseconds(100), [&] { return g_stop || g_sentN > last; });
            if (g_stop)
                return;
            target = g_sentN;
        }
        PresenterIdle();
        static ULONGLONG quietLogged = 0;
        const ULONGLONG now = GetTickCount64(), lastSend = g_lastSendTick;
        if (lastSend && now - lastSend > 2000 && now - quietLogged > 5000)
        {
            quietLogged = now;
            Log("presenter: no frame sent for %llu ms (game thread at: %s, frames sent %llu, ingested %llu)",
                now - lastSend, g_gameStage.load(), target, last);
        }
        Prune(grave, g_d);
        if (target <= last)
            continue;
        // The send lands before anything of this frame is queued on the display card
        QueryPerformanceCounter(&t0);
        g_presStage = "send wait";
        const bool landed = WaitValue(g_sendD.Get(), target, g_sendEv, 1000);
        QueryPerformanceCounter(&t1);
        const double ms = Ms(t0, t1, g_freq);
        {
            std::lock_guard sl(g_statM);
            if (ms > 250 || !landed)
                ++g_sendStalls;
            else
                g_sendWaitSum += ms, ++g_sendWaitN;
        }
        if (!landed || ms > 250)
        {
            static int logged = 0;
            if (logged++ < 30)
                Log("frame %llu: send %s after %.0f ms (render card hitch), nothing queued on the display card meanwhile",
                    target, landed ? "landed" : "NOT landed", ms);
            if (!landed)
            {
                DeviceLost("send wait");
                continue;
            }
        }
        const uint64_t n = g_sendD->GetCompletedValue(); // the newest frame sent: older ones are skipped
        Meta m;
        {
            std::lock_guard lk(g_pm);
            m = g_meta[n % K];
        }
        if (m.frame != n || !m.ring)
        {
            Log("frame %llu: slot holds frame %llu, skipped", n, m.frame);
            last = n;
            continue;
        }
        if (m.ring != held)
        {
            if (held)
                grave.push_back({ held, g_d.value });
            held = m.ring;
        }
        PresentFrame(n, m);
        last = n;
    }
    Log("presenter stopped: the game presents on its own from here (no NR)");
}

// Game thread: the back buffer into the ring. The real present is skipped while the presenter runs.
void MgpuSend(IDirect3DDevice9* dev, IDirect3DDevice9On12* d12)
{
    if (!g_presenterOn)
    {
        ComPtr<IDirect3DSwapChain9> sc;
        D3DPRESENT_PARAMETERS pp {};
        if (SUCCEEDED(dev->GetSwapChain(0, &sc)))
            sc->GetPresentParameters(&pp);
        D3DDEVICE_CREATION_PARAMETERS cp {};
        dev->GetCreationParameters(&cp);
        g_presentWnd = pp.hDeviceWindow ? pp.hDeviceWindow : cp.hFocusWindow;
        const int sync = IniInt(L"MgpuSyncInterval", -1);
        g_sync = sync >= 0 ? UINT(sync) : pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? 0u : 1u;
        Log("multi-GPU presenter starting: window %p, sync interval %u", g_presentWnd, g_sync);
        g_presenterOn = true;
        std::thread(PresenterLoop).detach();
    }
    ComPtr<IDirect3DSurface9> bb;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
    ComPtr<ID3D12Resource> res;
    g_gameStage = "unwrap";
    HRESULT hr = d12->UnwrapUnderlyingResource(bb.Get(), g_r.queue.Get(), IID_PPV_ARGS(&res));
    if (FAILED(hr))
    {
        static int n = 0;
        if (n++ < 5)
            Log("UnwrapUnderlyingResource failed: %08x", hr);
        return;
    }
    const auto desc = res->GetDesc();
    g_outW = (uint32_t) desc.Width, g_outH = desc.Height;
    static bool logged = false;
    if (!logged)
        logged = true, Log("back buffer %llux%u format %u samples %u flags %x", desc.Width, desc.Height, desc.Format,
                           desc.SampleDesc.Count, desc.Flags);
    Prune(g_graveR, g_r);
    bool ok = desc.SampleDesc.Count == 1;
    if (ok && (!g_ring || g_ring->desc.Width != desc.Width || g_ring->desc.Height != desc.Height ||
               g_ring->desc.Format != desc.Format))
    {
        if (g_ring)
            g_graveR.push_back({ g_ring, g_r.value });
        g_ring = MakeRing(desc);
    }
    ok = ok && g_ring;
    const uint64_t m = g_sentN + 1; // only this thread writes g_sentN
    if (ok && m > g_ahead)
    {
        // Slot m % K last held m - K; the presenter has taken m - g_ahead > m - K once this returns
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        g_gameStage = "slot wait";
        ok = WaitValue(g_ingest.Get(), m - g_ahead, g_ingestEv, g_timeoutMs);
        QueryPerformanceCounter(&t1);
        std::lock_guard sl(g_statM);
        if (ok)
            g_slotWaitSum += Ms(t0, t1, g_freq), ++g_slotWaitN;
        else
            ++g_dropped;
    }
    if (ok)
    {
        {
            std::lock_guard lk(g_pm);
            g_meta[m % K].ring = g_ring;
            g_meta[m % K].frame = m;
            QueryPerformanceCounter(&g_meta[m % K].t);
        }
        g_gameStage = "send copy";
        auto* l = Begin(g_r);
        Barrier(l, res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst { g_ring->slotR[m % K].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint = g_ring->fp;
        D3D12_TEXTURE_COPY_LOCATION src { res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(l, res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Submit(g_r);
        g_r.queue->Signal(g_sendR.Get(), m);
    }
    ++g_r.value;
    g_r.queue->Signal(g_r.local.Get(), g_r.value);
    ID3D12Fence* f = g_r.local.Get();
    UINT64 v = g_r.value;
    g_gameStage = "return";
    hr = d12->ReturnUnderlyingResource(bb.Get(), 1, &v, &f);
    g_gameStage = "in the game";
    if (FAILED(hr))
        Log("ReturnUnderlyingResource failed: %08x", hr);
    if (ok)
    {
        g_lastSendTick = GetTickCount64();
        {
            std::lock_guard lk(g_pm);
            g_sentN = m;
        }
        g_pcv.notify_one();
    }
}

bool MgpuOwnsPresent() { return g_mgpu && g_on12 && g_presenterOn && g_mgpuState != 2; }

void OnPresent(IDirect3DDevice9* dev)
{
    std::lock_guard lock(g_mutex);
    ++g_presents;
    Stats();
    PollFiles();
    if (g_iniDue && GetTickCount64() >= g_iniDue)
        g_iniDue = 0, SaveIni();
    if (g_presents == 60 && g_overlayOn && IniInt(L"StartupHint", 1))
        Overlay::Toast("lmxxf NR %s%s: Insert menu, End NR on/off, PgUp size, PgDn history, Home optical flow",
                       g_nrOn ? "on" : "OFF", g_mgpu ? " (2 GPUs)" : "");
    if (!g_on12)
        return;
    ComPtr<IDirect3DDevice9On12> d12;
    if (g_mgpu)
    {
        if (g_mgpuState == 2)
            return;
        const HRESULT q = dev->QueryInterface(IID_IDirect3DDevice9On12, (void**) d12.GetAddressOf());
        if (SUCCEEDED(q))
            MgpuSend(dev, d12.Get());
        else if (static int n = 0; n++ < 3)
            Log("multi-GPU: device %p is not a 9On12 device (%08x): nothing sent", dev, q);
        return;
    }
    bool send = RouteToHost();
    const bool overlay = g_overlayOn && Overlay::Visible();
    const bool dump = g_dumpPending;
    if (!send && !overlay && !dump)
        return;
    if (FAILED(dev->QueryInterface(IID_IDirect3DDevice9On12, (void**) d12.GetAddressOf())))
        return;
    ComPtr<IDirect3DSurface9> bb;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
    ComPtr<ID3D12Resource> res;
    HRESULT hr = d12->UnwrapUnderlyingResource(bb.Get(), g_r.queue.Get(), IID_PPV_ARGS(&res));
    if (FAILED(hr))
    {
        static int n = 0;
        if (n++ < 5)
            Log("UnwrapUnderlyingResource failed: %08x", hr);
        return;
    }
    const auto desc = res->GetDesc();
    g_outW = (uint32_t) desc.Width, g_outH = desc.Height;
    static bool logged = false;
    if (!logged)
        logged = true, Log("back buffer %llux%u format %u samples %u flags %x", desc.Width, desc.Height, desc.Format,
                           desc.SampleDesc.Count, desc.Flags);
    if (send && desc.SampleDesc.Count == 1 && EnsureFrame(g_r, desc))
    {
        Copy(g_r, g_frame.Get(), res.Get());
        if (HostRoundTrip())
            Copy(g_r, res.Get(), g_frame.Get());
    }
    if (overlay && desc.SampleDesc.Count == 1)
        DrawOverlay(g_r, res.Get(), desc);
    if (dump && desc.SampleDesc.Count == 1)
        g_dumpPending = false, DumpBackBuffer(g_r, res.Get(), desc);
    ++g_r.value;
    g_r.queue->Signal(g_r.local.Get(), g_r.value);
    ID3D12Fence* f = g_r.local.Get();
    UINT64 v = g_r.value;
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

// Single GPU: game present call -> real present (the host round trip included)
void CountLatency(const LARGE_INTEGER& t0)
{
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    std::lock_guard sl(g_statM);
    const double ms = Ms(t0, t1, g_freq);
    g_latSum += ms, ++g_latN, g_fpsLatSum += ms;
}

HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
    if (!(g_firstCalls.fetch_or(1) & 1))
        Log("first Present");
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    OnPresent(d);
    if (MgpuOwnsPresent())
        return S_OK;
    if (!g_mgpu)
        CountLatency(t0);
    const HRESULT hr = o_Present(d, a, b, w, r);
    if (FAILED(hr) && g_failedPresents++ < 10)
        Log("Present failed: %08x", hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkPresentEx(IDirect3DDevice9Ex* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r,
                                      DWORD f)
{
    if (!(g_firstCalls.fetch_or(2) & 2))
        Log("first PresentEx (flags %lx)", f);
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    OnPresent(d);
    if (MgpuOwnsPresent())
        return S_OK;
    if (!g_mgpu)
        CountLatency(t0);
    const HRESULT hr = o_PresentEx(d, a, b, w, r, f);
    if (FAILED(hr) && g_failedPresents++ < 10)
        Log("PresentEx failed: %08x", hr);
    return hr;
}

// MgpuFullscreen=1: the window over its monitor, the swapchain scaled. 2: also the game's display mode (CDS_FULLSCREEN),
// which loses the windowed 9On12 device for good (Crysis 2 stalls). Game thread, before the runtime's create / reset.
void SetGameMode(HWND w, UINT width, UINT height, UINT hz, bool mode)
{
    MONITORINFOEXW mi {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
    DEVMODEW cur {};
    cur.dmSize = sizeof(cur);
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur);
    LONG r = DISP_CHANGE_SUCCESSFUL;
    if (mode && (cur.dmPelsWidth != width || cur.dmPelsHeight != height || (hz && cur.dmDisplayFrequency != hz)))
    {
        DEVMODEW dm {};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = width, dm.dmPelsHeight = height;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        if (hz)
            dm.dmDisplayFrequency = hz, dm.dmFields |= DM_DISPLAYFREQUENCY;
        g_shieldUntil = GetTickCount64() + 60000;
        r = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr);
        g_shieldUntil = GetTickCount64() + 750;
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
    }
    const RECT& m = mi.rcMonitor;
    RECT wr {};
    GetWindowRect(w, &wr);
    if (r == DISP_CHANGE_SUCCESSFUL && !EqualRect(&wr, &m))
    {
        g_shieldUntil = GetTickCount64() + 1500; // async: the window thread handles it later
        SetWindowPos(w, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    g_modeSet = mode && r == DISP_CHANGE_SUCCESSFUL;
    Log("  %ls %ux%u@%u, game %ux%u@%u%s: %ld, window over the monitor %ldx%ld at %ld,%ld", mi.szDevice,
        cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency, width, height, hz,
        mode ? " (display mode switched to it)" : " (scaled)", r, m.right - m.left, m.bottom - m.top, m.left, m.top);
}

// Returns true when the game asked for exclusive fullscreen and it was turned into windowed (multi-GPU)
bool FixParams(D3DPRESENT_PARAMETERS* p, const char* where, HWND w = nullptr)
{
    if (p == nullptr)
        return false;
    Log("%s: %ux%u format %u msaa %u windowed %d swap %u interval %x buffers %u refresh %u", where, p->BackBufferWidth,
        p->BackBufferHeight, p->BackBufferFormat, p->MultiSampleType, p->Windowed, p->SwapEffect,
        p->PresentationInterval, p->BackBufferCount, p->FullScreen_RefreshRateInHz);
    if (!g_on12)
        return false;
    if (IniInt(L"ForceNoMsaa", 1) && p->MultiSampleType != D3DMULTISAMPLE_NONE)
        p->MultiSampleType = D3DMULTISAMPLE_NONE, p->MultiSampleQuality = 0, Log("  MSAA off (NR needs one sample)");
    // BGRX has no typed UAV: the host writes the frame texture
    if (p->BackBufferFormat == D3DFMT_X8R8G8B8 || p->BackBufferFormat == D3DFMT_UNKNOWN)
        p->BackBufferFormat = D3DFMT_A8R8G8B8, Log("  back buffer A8R8G8B8");
    // The render card has no outputs: the presenter's swapchain takes the fullscreen mode on the display card
    if (g_mgpu && !p->Windowed)
    {
        g_wantFs = true;
        g_fsW = p->BackBufferWidth, g_fsH = p->BackBufferHeight, g_fsHz = p->FullScreen_RefreshRateInHz;
        p->Windowed = TRUE;
        p->FullScreen_RefreshRateInHz = 0;
        Log("  windowed for 9On12 (multi-GPU: the display card presents the fullscreen frames, mode %d)", g_fsMode);
        const HWND win = p->hDeviceWindow ? p->hDeviceWindow : w;
        if ((g_fsMode == 1 || g_fsMode == 2) && win && p->BackBufferWidth > 64)
            SetGameMode(win, g_fsW, g_fsH, g_fsHz, g_fsMode == 2);
        return true;
    }
    return false;
}

// The runtime gets our parameters; the game's own struct gets the runtime's results back but keeps its fullscreen
// request (a game that reads Windowed back thinks the player switched to windowed: Crysis 2 reset itself and paused)
struct Params
{
    D3DPRESENT_PARAMETERS* game;
    D3DPRESENT_PARAMETERS copy {};
    bool windowedForUs = false;
    Params(D3DPRESENT_PARAMETERS* p, const char* where, HWND w = nullptr) : game(p)
    {
        if (p)
            copy = *p, windowedForUs = FixParams(&copy, where, w);
    }
    D3DPRESENT_PARAMETERS* Get() { return game ? &copy : nullptr; }
    bool Windowed() const { return game && copy.Windowed; }
    ~Params()
    {
        if (!game)
            return;
        const BOOL w = game->Windowed;
        const UINT hz = game->FullScreen_RefreshRateInHz;
        *game = copy;
        if (windowedForUs)
            game->Windowed = w, game->FullScreen_RefreshRateInHz = hz;
    }
};

using PFN_TestCoop = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
PFN_TestCoop o_TestCoop = nullptr;
HRESULT STDMETHODCALLTYPE hkTestCoop(IDirect3DDevice9* d)
{
    const HRESULT hr = o_TestCoop(d);
    static HRESULT last = S_OK;
    if (hr != last)
        Log("TestCooperativeLevel: %08x", hr), last = hr;
    return hr;
}

HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* p)
{
    D3DDEVICE_CREATION_PARAMETERS cp {};
    d->GetCreationParameters(&cp);
    Params q(p, "Reset", cp.hFocusWindow);
    return o_Reset(d, q.Get());
}
HRESULT STDMETHODCALLTYPE hkResetEx(IDirect3DDevice9Ex* d, D3DPRESENT_PARAMETERS* p, D3DDISPLAYMODEEX* m)
{
    D3DDEVICE_CREATION_PARAMETERS cp {};
    d->GetCreationParameters(&cp);
    Params q(p, "ResetEx", cp.hFocusWindow);
    return o_ResetEx(d, q.Get(), q.Windowed() ? nullptr : m); // windowed takes no display mode
}

void HookDevice(IDirect3DDevice9* dev, bool ex, HWND wnd)
{
    if (g_on12)
    {
        ComPtr<IDirect3DDevice9On12> d12;
        ComPtr<ID3D12Device> under;
        const HRESULT q = dev->QueryInterface(IID_IDirect3DDevice9On12, (void**) d12.GetAddressOf());
        if (SUCCEEDED(q))
            d12->GetD3D12Device(IID_PPV_ARGS(&under));
        const LUID l = under ? under->GetAdapterLuid() : LUID {};
        Log("  device %p: 9On12 %08x, its D3D12 device %s (LUID %08x:%08x)", dev, q,
            under.Get() == g_r.dev.Get() ? "ours" : under ? "OTHER" : "none", l.HighPart, l.LowPart);
    }
    void** vt = *(void***) dev;
    Patch(vt, 3, &hkTestCoop, &o_TestCoop);
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
    Subclass(wnd);
}

HWND DeviceWindow(HWND focus, const D3DPRESENT_PARAMETERS* p) { return p && p->hDeviceWindow ? p->hDeviceWindow : focus; }

HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                         D3DPRESENT_PARAMETERS* p, IDirect3DDevice9** out)
{
    Params q(p, "CreateDevice", w);
    t_inCreate = true;
    HRESULT hr = o_CreateDevice(d3d, a, t, w, f, q.Get(), out);
    t_inCreate = false;
    Log("CreateDevice adapter %u: %08x", a, hr);
    if (SUCCEEDED(hr) && out && *out)
    {
        ComPtr<IDirect3DDevice9Ex> ex;
        (*out)->QueryInterface(IID_PPV_ARGS(&ex));
        HookDevice(*out, ex != nullptr, DeviceWindow(w, p));
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateDeviceEx(IDirect3D9Ex* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                           D3DPRESENT_PARAMETERS* p, D3DDISPLAYMODEEX* m, IDirect3DDevice9Ex** out)
{
    Params q(p, "CreateDeviceEx", w);
    t_inCreate = true;
    HRESULT hr = o_CreateDeviceEx(d3d, a, t, w, f, q.Get(), q.Windowed() ? nullptr : m, out);
    t_inCreate = false;
    Log("CreateDeviceEx adapter %u: %08x", a, hr);
    if (SUCCEEDED(hr) && out && *out)
        HookDevice(*out, true, DeviceWindow(w, p));
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

// d3d9.dll and 9On12 take our D3D12 device only on its own adapter, and D3D9 adapters are the display card's outputs:
// while they set up (t_inCreate) the render-card device reports the display card's LUID
using PFN_GetAdapterLuid = LUID*(STDMETHODCALLTYPE*)(ID3D12Device*, LUID*);
PFN_GetAdapterLuid o_GetAdapterLuid = nullptr;
LUID g_displayLuid {};
std::atomic<int> g_luidLies { 0 };
constexpr int kGetAdapterLuidSlot = 43; // ID3D12Device vtable: IUnknown 3, ID3D12Object 4, then GetAdapterLuid is 37th

LUID* STDMETHODCALLTYPE hkGetAdapterLuid(ID3D12Device* d, LUID* out)
{
    o_GetAdapterLuid(d, out);
    if (t_inCreate && d == g_r.dev.Get())
    {
        if (g_luidLies++ < 4)
            Log("9On12 set-up: render card device reports the display LUID %08x:%08x", g_displayLuid.HighPart,
                g_displayLuid.LowPart);
        *out = g_displayLuid;
    }
    return out;
}

bool HookAdapterLuid()
{
    g_displayLuid = g_d.dev->GetAdapterLuid();
    void** vt = *(void***) g_r.dev.Get();
    o_GetAdapterLuid = (PFN_GetAdapterLuid) vt[kGetAdapterLuidSlot];
    DWORD old;
    if (!VirtualProtect(&vt[kGetAdapterLuidSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        return false;
    vt[kGetAdapterLuidSlot] = (void*) &hkGetAdapterLuid;
    VirtualProtect(&vt[kGetAdapterLuidSlot], sizeof(void*), old, &old);
    // Self-check: outside the set-up the real LUID still comes back
    const LUID l = g_r.dev->GetAdapterLuid();
    Log("ID3D12Device::GetAdapterLuid hooked, render device reads %08x:%08x outside 9On12 set-up", l.HighPart,
        l.LowPart);
    return !SameLuid(l, g_displayLuid);
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
    g_log = _wfsopen(g_logPath.c_str(), L"w", _SH_DENYNO); // tail-able while the game runs
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    g_sys = LoadLibraryW((std::wstring(sys) + L"\\d3d9.dll").c_str());
    g_enabled = IniInt(L"Enabled", 1) != 0;
    g_timeoutMs = (uint32_t) IniInt(L"TimeoutMs", 250);
    g_hotkeys = IniInt(L"Hotkeys", 1) != 0;
    g_overlayOn = IniInt(L"Overlay", 1) != 0;
    g_mgpu = IniInt(L"MultiGpu", 0) != 0;
    g_ahead = (uint32_t) std::clamp(IniInt(L"MgpuAhead", 1), 1, int(K) - 1);
    g_fsMode = std::clamp(IniInt(L"MgpuFullscreen", 1), 0, 3);
    for (int a = 0; a < kActions; ++a)
        g_keys[a] = (UINT) IniInt(kKeyNames[a], (int) g_keys[a]);
    g_dumpDir = IniStr(L"DumpDir", L"Host");
    if (!g_dumpDir.empty())
        g_bypassPath = g_dumpDir + L"\\proxy_bypass.trigger";
    LoadSettings();
    g_on12 = IniInt(L"NineOn12", 1) != 0 && g_sys && GetProcAddress(g_sys, "Direct3DCreate9On12Ex") && InitD3D12();
    if (!g_on12)
        g_mgpu = false;
    if (g_mgpu && !HookAdapterLuid())
        Log("multi-GPU: GetAdapterLuid hook failed: 9On12 will refuse the render card");
    Log("lmxxf legacy proxy: pid %lu, system d3d9 %p, 9On12 %d, NR %d (%s), hotkeys %d, overlay %d, multi-GPU %d",
        GetCurrentProcessId(), g_sys, g_on12, g_enabled, g_nrOn ? "on" : "bypassed", g_hotkeys, g_overlayOn, g_mgpu);
}

D3D9ON12_ARGS Args()
{
    D3D9ON12_ARGS a {};
    a.Enable9On12 = TRUE;
    a.pD3D12Device = g_r.dev.Get();
    a.ppD3D12Queues[0] = g_r.queue.Get();
    a.NumQueues = 1;
    return a;
}

template <typename T> T Sys(const char* name) { return g_sys ? (T) GetProcAddress(g_sys, name) : nullptr; }
} // namespace

namespace Overlay
{
Settings GetSettings()
{
    std::lock_guard lock(g_setMutex);
    return g_set;
}

void SetSettings(const Settings& s)
{
    {
        std::lock_guard lock(g_setMutex);
        g_set = s;
        g_nrOn = s.nr;
        PushControl(s.c);
    }
    g_iniDue = GetTickCount64() + 500;
}

Readouts GetReadouts()
{
    Readouts r;
    {
        std::lock_guard sl(g_statM);
        r.fps = g_mgpu ? g_shownFps : g_fps;
        r.gameFps = g_fps;
        r.latencyMs = g_latMs;
        r.proxyWaitMs = g_proxyWaitMs;
        r.presents = g_presents, r.timeouts = g_timeouts, r.skipped = g_skipped, r.bypassed = g_bypassed;
        r.dropped = g_dropped, r.occluded = g_occludedFrames;
    }
    r.frameMs = r.fps > 0 ? 1000.0f / r.fps : 0.0f;
    r.outW = g_outW, r.outH = g_outH;
    r.failedPresents = g_failedPresents.load();
    r.on12 = g_on12;
    r.mgpu = g_mgpu;
    r.mgpuOff = g_mgpuState == 2;
    r.hostUp = g_block && HostAlive();
    if (g_block)
    {
        const Legacy::Block& b = *g_block;
        r.hostMs = b.hostMs, r.netMs = b.netMs, r.ofMs = b.ofMs, r.waitMs = b.waitMs;
        r.feedW = b.feedWidth, r.feedH = b.feedHeight;
        r.nrFrames = b.nrFrames, r.passFrames = b.passFrames;
        r.hostState = b.hostState;
        r.caps = b.caps;
        r.applied = b.hostSerial == b.ctl.serial;
        r.flkStatic = b.flkStatic, r.flkMc = b.flkMc;
        memcpy(r.status, b.status, sizeof(r.status));
        r.status[sizeof(r.status) - 1] = 0;
        memcpy(r.error, b.error, sizeof(r.error));
        r.error[sizeof(r.error) - 1] = 0;
    }
    return r;
}
} // namespace Overlay

extern "C"
{
IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    Init();
    IDirect3D9* d = nullptr;
    if (g_on12)
    {
        auto a = Args();
        t_inCreate = true;
        d = Sys<PFN_Direct3DCreate9On12>("Direct3DCreate9On12")(sdk, &a, 1);
        t_inCreate = false;
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
        t_inCreate = true;
        hr = Sys<PFN_Direct3DCreate9On12Ex>("Direct3DCreate9On12Ex")(sdk, &a, 1, out);
        t_inCreate = false;
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
