// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
// Multi-GPU mode: the game renders on one card, the other card presents and runs NR. The mgpu/ modules do the work;
// this file connects them to the plugin API (adapter override, swapchain provider, present and upscale callbacks).
#include "MgpuPlugin.h"
#include "Autoconfig.h"
#include "Nr.h"
#include "mgpu/MgpuCore.h"
#include "GpuWait.h"

#include <detours/detours.h>
#include <intrin.h>

#include <cstdarg>
#include <filesystem>
#include <format>

extern const OptiPluginApi* g_api;
extern OptiPlugin* g_plugin;

namespace
{
bool g_on = false;
uint32_t g_colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
std::wstring g_dir;

HMODULE ModuleOf(const void* address)
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) address, &m);
    return m;
}

// Devices DXGI, D3D12, OptiScaler or this plugin create for themselves stay where they were asked for
bool SystemCaller(void* caller)
{
    HMODULE m = caller ? ModuleOf(caller) : nullptr;
    if (m == nullptr)
        return false;
    if (m == ModuleOf(&SystemCaller) || m == ModuleOf(g_api))
        return true;
    wchar_t p[MAX_PATH];
    if (!GetModuleFileNameW(m, p, MAX_PATH))
        return false;
    std::wstring n = std::filesystem::path(p).filename().wstring();
    for (auto& ch : n)
        ch = towlower(ch);
    return n == L"dxgi.dll" || n == L"d3d12.dll" || n == L"d3d12core.dll" || n == L"d3d11.dll" || n == L"dxcore.dll";
}

// The render card has no outputs: the game's EnumOutputs on it gets the display card's (HDR state, modes, fullscreen)
using PFN_EnumOutputs = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter*, UINT, IDXGIOutput**);
PFN_EnumOutputs o_EnumOutputs = nullptr;

HRESULT STDMETHODCALLTYPE hkEnumOutputs(IDXGIAdapter* self, UINT n, IDXGIOutput** out)
{
    const HRESULT hr = o_EnumOutputs(self, n, out);
    if (hr != DXGI_ERROR_NOT_FOUND || out == nullptr || SystemCaller(_ReturnAddress()))
        return hr;
    IDXGIAdapter1* display = Mgpu::DisplayAdapter();
    DXGI_ADAPTER_DESC d {};
    const LUID r = Mgpu::RenderLuid();
    if (display == nullptr || self->GetDesc(&d) != S_OK || d.AdapterLuid.LowPart != r.LowPart ||
        d.AdapterLuid.HighPart != r.HighPart)
        return hr;
    const HRESULT hd = o_EnumOutputs(display, n, out);
    static bool logged = false;
    if (!logged && hd == S_OK)
    {
        logged = true;
        Log(OptiLog_Info, "[mgpu] render adapter EnumOutputs(%u): the display card's output", n);
    }
    return hd;
}

void InstallOutputForward()
{
    static bool done = false;
    IDXGIAdapter1* display = Mgpu::DisplayAdapter();
    if (done || display == nullptr)
        return;
    done = true;
    o_EnumOutputs = reinterpret_cast<PFN_EnumOutputs>((*reinterpret_cast<void***>(display))[7]);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_EnumOutputs, hkEnumOutputs);
    const LONG err = DetourTransactionCommit();
    Log(err == NO_ERROR ? OptiLog_Info : OptiLog_Error, "[mgpu] output forward for the render adapter: %s (%ld)",
        err == NO_ERROR ? "on" : "failed", err);
}

void* AdapterOverride(void* requested, void* caller, void*)
{
    if (SystemCaller(caller))
        return nullptr;
    IUnknown* a = Mgpu::RedirectAdapter(static_cast<IUnknown*>(requested));
    if (Mgpu::Redirected() && Mgpu::CurrentSettings().forwardOutputs)
        InstallOutputForward();
    // The render card, asked for or redirected to: the game's device
    return Mgpu::Redirected() ? a : nullptr;
}

HRESULT STDMETHODCALLTYPE CreateUnhooked(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
                                         const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                         IDXGIOutput* restrict, IDXGISwapChain1** out)
{
    return g_api->CreateSwapChainForHwndUnhooked(factory, device, hwnd, desc, fs, restrict, (void**) out);
}

int32_t SwapchainProvider(void* factory, void* queue, void* hwnd, const void* desc, const void* fs, void* restrict,
                          void** swapChain, void*)
{
    const auto& s = Mgpu::CurrentSettings();
    if (!s.enabled || !s.presenter || !Mgpu::Redirected() || queue == nullptr)
        return E_NOTIMPL;
    ID3D12CommandQueue* q = nullptr;
    if (static_cast<IUnknown*>(queue)->QueryInterface(IID_PPV_ARGS(&q)) != S_OK)
        return E_NOTIMPL;
    q->Release();
    return Mgpu::CreateSwapChainForHwnd(CreateUnhooked, static_cast<IDXGIFactory2*>(factory), q, (HWND) hwnd,
                                        static_cast<const DXGI_SWAP_CHAIN_DESC1*>(desc),
                                        static_cast<const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*>(fs),
                                        static_cast<IDXGIOutput*>(restrict), (IDXGISwapChain1**) swapChain);
}

uint32_t Encoding(uint32_t colorSpace)
{
    if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
        return 2;
    if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
        return 3;
    return 1;
}

bool NrFrame(ID3D12Device* d, ID3D12CommandQueue* q, const Mgpu::NrInput& in)
{
    MgpuEnsureRuntime(DisplayEngine());
    NrEngine::OwnFrame f;
    f.src = in.src;
    f.srcState = in.srcState;
    f.dst = in.dst;
    f.dstState = in.dstState;
    f.motion = in.motion;
    f.motionState = in.motionState;
    f.motionW = in.guides.motionW;
    f.motionH = in.guides.motionH;
    f.mvScaleX = in.guides.mvScaleX;
    f.mvScaleY = in.guides.mvScaleY;
    f.reset = in.reset;
    f.waitAnswer = in.waitAnswer;
    f.waitMs = in.answerWaitMs;
    f.encoding = Encoding(g_colorSpace);
    return DisplayEngine().RunOwn(d, q, f);
}

// Our own list on the game's queue, executed right before the proxy's Present: the HUD-less pick of this frame
struct CaptureLists
{
    ComPtr<ID3D12CommandAllocator> alloc[4];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t value[4] = {};
    uint64_t counter = 0;
    HANDLE event = nullptr;
} g_cap;

void CaptureHudless(OptiPresentFrame* f, void* res, uint32_t state, bool standIn)
{
    auto* dev = static_cast<ID3D12Device*>(f->device);
    auto* q = static_cast<ID3D12CommandQueue*>(f->queue);
    if (!g_cap.list)
    {
        for (auto& a : g_cap.alloc)
            dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cap.alloc[0].Get(), nullptr,
                               IID_PPV_ARGS(&g_cap.list));
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_cap.fence));
        g_cap.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_cap.list || !g_cap.fence)
            return;
        g_cap.list->Close();
    }
    const uint32_t s = uint32_t(g_cap.counter % 4);
    // the slot's capture list must be done before its allocator is reset: else no capture this frame
    if (!GpuWaitDone(g_cap.fence.Get(), g_cap.value[s]))
        return Log(OptiLog_Warn, "[mgpu] HUD-less capture skipped: its list from 4 frames ago is not done");
    g_cap.alloc[s]->Reset();
    g_cap.list->Reset(g_cap.alloc[s].Get(), nullptr);
    Mgpu::CaptureHudless(g_cap.list.Get(), static_cast<ID3D12Resource*>(res), (D3D12_RESOURCE_STATES) state, standIn);
    g_cap.list->Close();
    ID3D12CommandList* l[] = { g_cap.list.Get() };
    o_ExecuteCommandLists ? o_ExecuteCommandLists(q, 1, l) : q->ExecuteCommandLists(1, l);
    g_cap.value[s] = ++g_cap.counter;
    q->Signal(g_cap.fence.Get(), g_cap.value[s]);
}

int g_standIn = 1; // MgpuMenuStandIn: 0 off, 1 while the menu is open, 2 every frame (the FPS overlay too)

std::string Busy(double v) { return v < 0 ? std::string("-") : std::format("{:.0f}%", v); }
} // namespace

void MgpuEnsureRuntime(NrEngine& e)
{
    static std::once_flag once;
    std::call_once(once, [&] { MgpuLoadRuntime(e); });
}

bool MgpuActive() { return g_on && Mgpu::Redirected(); }

bool MgpuFallback() { return g_on && Mgpu::GetStats().fallback; }

bool MgpuInit(const OptiPluginApi* api, OptiPlugin* plugin, const std::wstring& dumpDir)
{
    Mgpu::Settings s;
    s.enabled = g_keys.Bool("MultiGpu", false);
    if (!s.enabled)
        return false;
    if (!OPTI_API_HAS(api, RegisterAdapterOverride) || !OPTI_API_HAS(api, RegisterSwapchainProvider) ||
        !OPTI_API_HAS(api, CreateD3D12DeviceUnhooked) || !OPTI_API_HAS(api, CreateSwapChainForHwndUnhooked))
    {
        Log(OptiLog_Error, "multi-GPU needs the adapter override and swapchain provider API: off");
        return false;
    }
    s.renderBus = g_keys.Int("MgpuRenderBus", -1);
    s.displayBus = g_keys.Int("MgpuDisplayBus", -1);
    s.hudless = g_keys.Bool("MgpuHudless", true);
    s.wholeFrameFallback = g_keys.Bool("WholeFrameFallback", true);
    s.presenter = g_keys.Bool("MgpuPresenter", true);
    s.fallbackInline = g_keys.Bool("MgpuFallbackInline", true);
    s.nrEveryFrame = g_keys.Bool("MgpuNrEveryFrame", true);
    s.temporal = g_keys.Bool("MgpuMotionVectors", true);
    s.uiGate = std::clamp(g_keys.Float("MgpuUiGate", 0.90f), 0.0f, 1.0f);
    s.maxLead = (unsigned) std::clamp(g_keys.Int("MgpuMaxFramesAhead", 1), 1, 3);
    s.lowLatency = std::clamp(g_keys.Int("MgpuLowLatency", 2), 0, 2);
    s.earlySend = g_keys.Bool("MgpuEarlySend", false);
    s.presentIdle = g_keys.Bool("MgpuPresentIdle", false);
    // MgpuDred: true / 1 breadcrumbs + page faults, 2 page faults only (breadcrumbs serialise the queue)
    s.dred = std::clamp(g_keys.Int("MgpuDred", g_keys.Bool("MgpuDred", false) ? 1 : 0), 0, 2);
    s.forwardOutputs = g_keys.Bool("MgpuForwardOutputs", true);
    g_standIn = std::clamp(g_keys.Int("MgpuMenuStandIn", 1), 0, 2);

    g_dir = dumpDir;
    Mgpu::Env e;
    e.log = [](int level, const std::string& m)
    { Log(level >= 2 ? OptiLog_Error : level == 1 ? OptiLog_Warn : OptiLog_Info, "[mgpu] %s", m.c_str()); };
    e.createFactory = []() -> IDXGIFactory1*
    {
        IDXGIFactory1* f = nullptr;
        return SUCCEEDED(g_api->CreateDxgiFactoryUnhooked(&__uuidof(IDXGIFactory1), (void**) &f)) ? f : nullptr;
    };
    e.createDevice = [](IUnknown* adapter, REFIID riid, void** device) -> HRESULT
    { return g_api->CreateD3D12DeviceUnhooked(adapter, D3D_FEATURE_LEVEL_12_0, &riid, device); };
    e.noSpoof = [](const std::function<void()>& fn)
    {
        if (OPTI_API_HAS(g_api, RunUnspoofed))
            g_api->RunUnspoofed([](void* u) { (*static_cast<const std::function<void()>*>(u))(); }, (void*) &fn);
        else
            fn();
    };
    e.nrFrame = NrFrame;
    e.nrSettle = [] { return DisplayEngine().SettleOwn(); };
    e.hookExecute = [](ID3D12Device* d)
    {
        extern bool InstallQueueHook(ID3D12Device * device);
        InstallQueueHook(d);
    };
    e.nrQuiesce = [] { DisplayEngine().Quiesce(); };
    e.nrIdle = [](const char* why) { DisplayEngine().SeqIdle(why); };
    e.nrWanted = [] { return Engine().Settings().enabled; };
    // jobs since the last call x the network's GPU time, over the wall time (the load sampler's thread only)
    e.nrBusy = []() -> double
    {
        static uint64_t lastRun = 0;
        static std::chrono::steady_clock::time_point last {};
        const auto st = DisplayEngine().Stats();
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - last).count();
        const uint64_t jobs = st.framesRun >= lastRun ? st.framesRun - lastRun : 0;
        const bool first = !last.time_since_epoch().count();
        last = now, lastRun = st.framesRun;
        if (first || ms <= 0)
            return -1;
        return std::min(100.0, double(jobs) * st.hipMs / ms * 100.0);
    };
    e.nrStatus = []
    {
        const auto st = DisplayEngine().Stats();
        return std::format("display NR {} {}x{} -> {}x{}, {:.2f} ms, {} run {} skipped {}", st.state, st.srcWidth,
                           st.srcHeight, st.feedWidth, st.feedHeight, st.hipMs, st.framesRun, st.framesSkipped,
                           st.error);
    };
    e.dumpDir = dumpDir;
    Mgpu::Configure(s, e);
    DisplayEngine().name = L"display";
    DisplayEngine().dumpDir = dumpDir;

    api->RegisterAdapterOverride(plugin, AdapterOverride, nullptr);
    api->RegisterSwapchainProvider(plugin, SwapchainProvider, nullptr);
    g_on = true;
    Log(OptiLog_Info, "multi-GPU on: hudless %d, NR every frame %d, motion vectors %d, UI gate %.2f, frames ahead %u",
        s.hudless, s.nrEveryFrame, s.temporal, s.uiGate, s.maxLead);
    return true;
}

bool MgpuWantsHudless() { return g_on && Mgpu::CurrentSettings().hudless; }

uint32_t MgpuPresentRequests()
{
    return MgpuWantsHudless() ? OptiPresentRequest_Hudless | OptiPresentRequest_PreOverlay : 0;
}

void MgpuOnPresent(OptiPresentFrame* f)
{
    if (!g_on)
        return;
    g_colorSpace = f->colorSpace;
    static bool fgHud = false; // this frame's HUD-less colour came at the game's FG swapchain, before PreOverlay
    if (f->phase == OptiPresent_AfterOverlay && f->hudless != nullptr && Mgpu::WantHudless())
    {
        CaptureHudless(f, f->hudless, f->hudlessState, false);
        fgHud = f->fgSwapchain != 0;
    }
    else if (f->phase == OptiPresent_PreOverlay)
    {
        // No HUD-less colour and the menu about to be drawn: the frame as it is now stands in for one, so the
        // whole-frame fallback runs on it and the menu is composited back like a HUD
        const bool real = f->hudless != nullptr || std::exchange(fgHud, false);
        const bool menu = g_standIn == 2 || (g_standIn == 1 && OPTI_API_HAS(g_api, IsMenuOpen) && g_api->IsMenuOpen());
        if (!real && menu && Mgpu::WantHudless() && f->backBuffer != nullptr)
            CaptureHudless(f, f->backBuffer, f->backBufferState, true);
    }
}

void MgpuOnUpscale(OptiUpscaleFrame* f)
{
    if (!g_on || !Mgpu::WantGuides() || f->motionVectors == nullptr)
        return;
    Mgpu::Guides g;
    g.motion = static_cast<ID3D12Resource*>(f->motionVectors);
    g.motionState = (D3D12_RESOURCE_STATES) f->motionVectorsState;
    g.renderW = f->renderWidth;
    g.renderH = f->renderHeight;
    const bool display = (f->flags & OptiUpscale_DisplayResMV) != 0;
    g.motionW = display ? f->outputWidth : f->renderWidth;
    g.motionH = display ? f->outputHeight : f->renderHeight;
    g.mvScaleX = f->mvScaleX;
    g.mvScaleY = f->mvScaleY;
    g.jitterX = f->jitterX;
    g.jitterY = f->jitterY;
    g.reset = (f->flags & OptiUpscale_Reset) != 0;
    g.depthInverted = (f->flags & OptiUpscale_DepthInverted) != 0;
    Mgpu::CaptureGuides(static_cast<ID3D12GraphicsCommandList*>(f->commandList), g);
}

void MgpuDrawTab(const OptiPluginApi* api)
{
    if (!g_on)
    {
        api->TextDisabled("Off ([Plugin.lmxxf_nr] MultiGpu=true and a restart turn it on).");
        return;
    }
    const auto st = Mgpu::GetStats();
    const auto& s = Mgpu::CurrentSettings();
    auto line = [&](const std::string& t) { api->Text(t.c_str()); };
    line(std::format("State: {}{}", st.state, Mgpu::Redirected() ? "" : " (game device not redirected)"));
    if (st.fallback)
        api->TextColored(1.0f, 0.45f, 0.4f, 1.0f,
                         ("Display card lost (" + st.fallbackReason + "): single-GPU passthrough on the render card, NR off").c_str());
    if (!st.output.empty())
        line("Display output: " + st.output);
    line(std::format("Render card: {} (bus {}), {} busy", st.render.name, st.render.bus, Busy(st.renderBusy)));
    line(std::format("Display card: {} (bus {}), {} busy (D3D {}, network {})", st.display.name, st.display.bus,
                     Busy(st.displayBusy), Busy(st.displayD3dBusy), Busy(st.displayNetBusy)));
    line(std::format("Mode: {}, NR every frame {}, motion vectors {}, frames ahead {}", s.hudless ? "HUD-less" : "whole frame",
                     s.nrEveryFrame ? "on" : "off", s.temporal ? "on" : "off", s.maxLead));
    line(std::format("Frame {}x{}, render {:.2f} ms, display {:.2f} ms, added latency {:.2f} ms", st.width, st.height,
                     st.renderFrameMs, st.displayFrameMs, st.latencyMs));
    line(std::format("Send {:.2f} ms ({:.1f} GB/s), ingest {:.2f} ms ({:.1f} GB/s), CPU waits send {:.2f} ms answer {:.2f} ms",
                     st.sendMs, st.sendGBs, st.ingestMs, st.ingestGBs, st.sendWaitMs, st.answerWaitMs));
    line(std::format("Frames sent {}, presented {}, skipped {}", st.produced, st.presented, st.skipped));
    if (s.hudless)
        line(std::format("HUD-less frames {}, missed {}, UI {:.1f}% of pixels, gate trips {}", st.hudlessFrames,
                         st.hudlessMissed, st.uiShare * 100, st.gateFallbacks));
    if (!st.nrSource.empty())
        line("NR source: " + st.nrSource);
    if (!st.nrStatus.empty())
        api->TextDisabled(st.nrStatus.c_str());
}

bool MgpuOverlayText(char* buf, size_t size)
{
    if (!g_on || !Mgpu::Redirected())
        return false;
    const auto st = Mgpu::GetStats();
    const auto nr = DisplayEngine().Stats();
    if (st.fallback)
    {
        snprintf(buf, size, "display card lost: single-GPU present, NR off");
        static bool logged = false;
        if (!logged)
            logged = true, Log(OptiLog_Warn, "[mgpu] overlay: %s", buf);
        return true;
    }
    snprintf(buf, size, "render %s, display %s, +%.1f ms, NR %.1f ms", Busy(st.renderBusy).c_str(),
             Busy(st.displayBusy).c_str(), st.latencyMs, nr.hipMs);
    return true;
}
