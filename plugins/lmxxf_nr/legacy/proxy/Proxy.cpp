// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Legacy mode, game side: a 32-bit d3d9.dll proxy. The game's D3D9(Ex) device runs on D3D9On12 over a D3D12 device
// of ours; at each present the back buffer goes to the x64 NR host through a shared texture and fence and back.
// Hotkeys and the menu (Overlay.cpp) write the host's settings into the shared block and the ini.
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <d3d9on12.h>
#include <dxgi1_4.h>
#include <share.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

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

// D3D12 side
ComPtr<ID3D12Device> g_dev;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12Fence> g_shared;  // with the host, see LegacyShared.h
ComPtr<ID3D12Fence> g_local;   // return fence for 9On12
uint64_t g_localValue = 0;
HANDLE g_sharedHandle = nullptr, g_event = nullptr, g_localEvent = nullptr;
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
bool g_wasSending = true;

// Overlay drawing: own allocators (3 frames in flight, as the ImGui backend) and an RTV for the back buffer
bool g_overlayOn = true, g_overlayFailed = false;
DXGI_FORMAT g_overlayFormat = DXGI_FORMAT_UNKNOWN;
ComPtr<ID3D12CommandAllocator> g_ovAlloc[3];
ComPtr<ID3D12GraphicsCommandList> g_ovList;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
uint64_t g_ovValue[3] = {};
uint32_t g_ovNext = 0;
bool g_dumpPending = false;

// Stats
std::atomic<uint64_t> g_failedPresents { 0 };
uint64_t g_presents = 0, g_sent = 0, g_returned = 0, g_timeouts = 0, g_skipped = 0, g_bypassed = 0;
LARGE_INTEGER g_freq {}, g_lastStat {};
double g_waitMsSum = 0;
uint32_t g_outW = 0, g_outH = 0;
// Readouts window (500 ms)
LARGE_INTEGER g_lastFps {};
uint64_t g_fpsFrames = 0, g_fpsReturned = 0;
double g_fpsWaitSum = 0;
float g_fps = 0, g_proxyWaitMs = 0;

// Settings: the menu / hotkeys own them, the host applies them from the block
std::mutex g_setMutex; // never held while calling into Overlay
Overlay::Settings g_set;
std::atomic<bool> g_nrOn { true };
ULONGLONG g_iniDue = 0; // save the ini at this tick (0: nothing pending)

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

LRESULT CALLBACK SubProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
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
    if (w == nullptr || g_wnd != nullptr || !g_hotkeys)
        return;
    g_wnd = w;
    g_origProc = (WNDPROC) SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR) &SubProc);
    Log("window %p subclassed (%s), keys: menu %u, NR %u, size %u, history %u, flow %u", w,
        g_origProc ? "ok" : "FAILED", g_keys[kMenu], g_keys[kNr], g_keys[kSize], g_keys[kHistory], g_keys[kFlow]);
    if (g_origProc == nullptr)
        g_wnd = nullptr;
}

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
    g_localEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

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
    {
        std::lock_guard lock(g_setMutex);
        PushControl(g_set.c);
    }
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

// Copies src -> dst on our queue (both COMMON at the boundaries)
void Copy(ID3D12Resource* dst, ID3D12Resource* src)
{
    const uint32_t i = g_allocNext++ & 1;
    WaitValue(g_local.Get(), g_allocValue[i], g_localEvent, 1000);
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

// Menu / toast onto the back buffer, on our queue before it goes back to 9On12
void DrawOverlay(ID3D12Resource* bb, const D3D12_RESOURCE_DESC& desc)
{
    if (g_overlayFailed)
        return;
    const DXGI_FORMAT fmt = RtvFormat(desc.Format);
    if (g_overlayFormat != fmt)
    {
        if (g_overlayFormat != DXGI_FORMAT_UNKNOWN)
        {
            WaitValue(g_local.Get(), g_localValue, g_localEvent, 1000);
            Overlay::Shutdown();
        }
        if (!g_ovList)
        {
            for (auto& a : g_ovAlloc)
                g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
            g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ovAlloc[0].Get(), nullptr,
                                     IID_PPV_ARGS(&g_ovList));
            D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
            g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
            if (g_ovList)
                g_ovList->Close();
        }
        if (!g_ovList || !g_rtvHeap || !Overlay::Init(g_dev.Get(), g_queue.Get(), fmt, desc.Height))
        {
            g_overlayFailed = true;
            Log("overlay init failed (format %u): toasts and menu off, hotkeys still work", fmt);
            return;
        }
        g_overlayFormat = fmt;
        Log("overlay ready (format %u, %llux%u)", fmt, desc.Width, desc.Height);
    }
    const uint32_t i = g_ovNext++ % 3;
    WaitValue(g_local.Get(), g_ovValue[i], g_localEvent, 1000);
    g_ovAlloc[i]->Reset();
    g_ovList->Reset(g_ovAlloc[i].Get(), nullptr);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rd {};
    rd.Format = fmt;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_dev->CreateRenderTargetView(bb, &rd, rtv);
    Barrier(g_ovList.Get(), bb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Overlay::Draw(g_ovList.Get(), bb, rtv, (uint32_t) desc.Width, desc.Height);
    Barrier(g_ovList.Get(), bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    g_ovList->Close();
    ID3D12CommandList* l[] = { g_ovList.Get() };
    g_queue->ExecuteCommandLists(1, l);
    g_ovValue[i] = ++g_localValue;
    g_queue->Signal(g_local.Get(), g_localValue);
}

// <DumpDir>\proxy_dump.trigger: the back buffer as presented (overlay included) to <DumpDir>\proxy_frame.bmp
void DumpBackBuffer(ID3D12Resource* bb, const D3D12_RESOURCE_DESC& desc)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    g_dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&rb))))
        return;
    const uint32_t i = g_allocNext++ & 1;
    WaitValue(g_local.Get(), g_allocValue[i], g_localEvent, 1000);
    g_alloc[i]->Reset();
    g_list->Reset(g_alloc[i].Get(), nullptr);
    Barrier(g_list.Get(), bb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst { rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src { bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(g_list.Get(), bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    g_list->Close();
    ID3D12CommandList* l[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, l);
    g_allocValue[i] = ++g_localValue;
    g_queue->Signal(g_local.Get(), g_localValue);
    if (!WaitValue(g_local.Get(), g_localValue, g_localEvent, 2000))
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
    if (g_lastStat.QuadPart == 0)
        g_lastStat = g_lastFps = now;
    const double fs = double(now.QuadPart - g_lastFps.QuadPart) / double(g_freq.QuadPart);
    if (fs >= 0.5)
    {
        const uint64_t ret = g_returned - g_fpsReturned;
        g_fps = float(double(g_presents - g_fpsFrames) / fs);
        g_proxyWaitMs = ret ? float(g_fpsWaitSum / double(ret)) : 0.0f;
        g_fpsFrames = g_presents, g_fpsReturned = g_returned, g_fpsWaitSum = 0, g_lastFps = now;
    }
    const double s = double(now.QuadPart - g_lastStat.QuadPart) / double(g_freq.QuadPart);
    if (s < 4.0)
        return;
    const uint64_t frames = g_presents;
    static uint64_t lastFrames = 0, lastReturned = 0;
    const double fps = double(frames - lastFrames) / s;
    const uint64_t ret = g_returned - lastReturned;
    Log("pacing: %.1f fps, %llu presents, %llu returned from the host (%llu in window), %llu timeouts, %llu skipped, "
        "%llu failed presents, host wait %.2f ms/frame, host %s state %d gpu %.2f ms, of %.2f ms, flk %.5f / %.5f, %s, "
        "%llu bypassed, net %.2f ms",
        fps, frames, g_returned, ret, g_timeouts, g_skipped, g_failedPresents.load(), ret ? g_waitMsSum / double(ret) : 0.0,
        HostAlive() ? "up" : "down", g_block ? g_block->hostState : -1, g_block ? g_block->hostMs : 0.f,
        g_block ? g_block->ofMs : 0.f, g_block ? g_block->flkStatic : 0.f, g_block ? g_block->flkMc : 0.f,
        g_block ? g_block->status : "", g_bypassed, g_block ? g_block->netMs : 0.f);
    if (g_dev && g_shared)
        Log("  device %08x, shared fence %llu, frame %llu", (unsigned) g_dev->GetDeviceRemovedReason(),
            g_shared->GetCompletedValue(), g_n);
    lastFrames = frames;
    lastReturned = g_returned;
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

void OnPresent(IDirect3DDevice9* dev)
{
    std::lock_guard lock(g_mutex);
    ++g_presents;
    Stats();
    PollFiles();
    if (g_iniDue && GetTickCount64() >= g_iniDue)
        g_iniDue = 0, SaveIni();
    if (g_presents == 60 && g_overlayOn && IniInt(L"StartupHint", 1))
        Overlay::Toast("lmxxf NR %s: Insert menu, End NR on/off, PgUp size, PgDn history, Home optical flow",
                       g_nrOn ? "on" : "OFF");
    if (!g_on12)
        return;
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
    const bool overlay = g_overlayOn && Overlay::Visible();
    const bool dump = g_dumpPending;
    if (!send && !overlay && !dump)
        return;
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
    g_outW = (uint32_t) desc.Width, g_outH = desc.Height;
    static bool logged = false;
    if (!logged)
        logged = true, Log("back buffer %llux%u format %u samples %u flags %x", desc.Width, desc.Height, desc.Format,
                           desc.SampleDesc.Count, desc.Flags);
    if (send && desc.SampleDesc.Count == 1 && EnsureFrame(desc))
    {
        Copy(g_frame.Get(), res.Get());
        ++g_n;
        g_block->frame = g_n;
        MemoryBarrier();
        g_queue->Signal(g_shared.Get(), 2 * g_n - 1);
        ++g_sent;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const bool back = WaitValue(g_shared.Get(), 2 * g_n, g_event, g_timeoutMs);
        QueryPerformanceCounter(&t1);
        if (back)
        {
            Copy(res.Get(), g_frame.Get());
            ++g_returned;
            const double ms = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(g_freq.QuadPart);
            g_waitMsSum += ms;
            g_fpsWaitSum += ms;
        }
        else
        {
            g_stalled = true;
            if (g_timeouts++ < 10)
                Log("frame %llu: host did not answer within %u ms", g_n, g_timeoutMs);
        }
    }
    if (overlay && desc.SampleDesc.Count == 1)
        DrawOverlay(res.Get(), desc);
    if (dump && desc.SampleDesc.Count == 1)
        g_dumpPending = false, DumpBackBuffer(res.Get(), desc);
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
    OnPresent(d);
    const HRESULT hr = o_PresentEx(d, a, b, w, r, f);
    if (FAILED(hr) && g_failedPresents++ < 10)
        Log("PresentEx failed: %08x", hr);
    return hr;
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

void HookDevice(IDirect3DDevice9* dev, bool ex, HWND wnd)
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
    Subclass(wnd);
}

HWND DeviceWindow(HWND focus, const D3DPRESENT_PARAMETERS* p) { return p && p->hDeviceWindow ? p->hDeviceWindow : focus; }

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
        HookDevice(*out, ex != nullptr, DeviceWindow(w, p));
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
    for (int a = 0; a < kActions; ++a)
        g_keys[a] = (UINT) IniInt(kKeyNames[a], (int) g_keys[a]);
    g_dumpDir = IniStr(L"DumpDir", L"Host");
    if (!g_dumpDir.empty())
        g_bypassPath = g_dumpDir + L"\\proxy_bypass.trigger";
    LoadSettings();
    g_on12 = IniInt(L"NineOn12", 1) != 0 && g_sys && GetProcAddress(g_sys, "Direct3DCreate9On12Ex") && InitD3D12();
    Log("lmxxf legacy proxy: pid %lu, system d3d9 %p, 9On12 %d, NR %d (%s), hotkeys %d, overlay %d",
        GetCurrentProcessId(), g_sys, g_on12, g_enabled, g_nrOn ? "on" : "bypassed", g_hotkeys, g_overlayOn);
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
    r.fps = g_fps;
    r.frameMs = g_fps > 0 ? 1000.0f / g_fps : 0.0f;
    r.proxyWaitMs = g_proxyWaitMs;
    r.outW = g_outW, r.outH = g_outH;
    r.presents = g_presents, r.timeouts = g_timeouts, r.skipped = g_skipped, r.bypassed = g_bypassed;
    r.failedPresents = g_failedPresents.load();
    r.on12 = g_on12;
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
