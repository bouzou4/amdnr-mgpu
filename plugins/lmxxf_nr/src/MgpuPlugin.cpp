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

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <vector>
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

void CaptureHudless(OptiPresentFrame* f)
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
        for (auto& a : g_cap.alloc)
            if (a)
                a->SetName(L"Mgpu hudless capture allocator");
        g_cap.list->SetName(L"Mgpu hudless capture list");
        g_cap.list->Close();
    }
    const uint32_t s = uint32_t(g_cap.counter % 4);
    // the slot's capture list must be done before its allocator is reset: else no capture this frame
    if (!GpuWaitDone(g_cap.fence.Get(), g_cap.value[s]))
        return Log(OptiLog_Warn, "[mgpu] HUD-less capture skipped: its list from 4 frames ago is not done");
    g_cap.alloc[s]->Reset();
    g_cap.list->Reset(g_cap.alloc[s].Get(), nullptr);
    Mgpu::CaptureHudless(g_cap.list.Get(), static_cast<ID3D12Resource*>(f->hudless),
                         (D3D12_RESOURCE_STATES) f->hudlessState);
    g_cap.list->Close();
    ID3D12CommandList* l[] = { g_cap.list.Get() };
    o_ExecuteCommandLists ? o_ExecuteCommandLists(q, 1, l) : q->ExecuteCommandLists(1, l);
    g_cap.value[s] = ++g_cap.counter;
    q->Signal(g_cap.fence.Get(), g_cap.value[s]);
}

std::string Busy(double v) { return v < 0 ? std::string("-") : std::format("{:.0f}%", v); }

// One card's link: the slot's PCIe link from the driver, else what the measured copy rate amounts to.
// Copies reach about 90 % of a link's raw rate (Gen5 x8: 28.4 of 31.5 GB/s, Gen4 x8: 14.1 of 15.8).
struct Link
{
    bool known = false;
    int gen = 0, lanes = 0;
    double gbs = 0;      // what copies can reach on it
    double measured = 0; // GB/s of this card's copy (render: send, display: ingest)
    std::string Short() const
    {
        if (known)
            return std::format("Gen{} x{}", gen, lanes);
        return measured > 0 ? std::format("~{:.0f} GB/s", measured) : "?";
    }
};
constexpr double kCopyEff = 0.9;
Link LinkOf(const Mgpu::AdapterView& a, double measured)
{
    Link l;
    l.measured = measured;
    if (a.pcieGen > 0 && a.pcieLanes > 0)
    {
        l.known = true, l.gen = a.pcieGen, l.lanes = a.pcieLanes;
        l.gbs = Mgpu::LinkGBs(l.gen, l.lanes) * kCopyEff;
    }
    else
        l.gbs = measured;
    return l;
}
// the x8 generation a measured rate matches (for an unknown link)
std::string ClassOf(double gbs)
{
    int best = 0;
    double d = 1e9;
    for (int g = 3; g <= 5; ++g)
    {
        const double e = std::abs(std::log(gbs / (Mgpu::LinkGBs(g, 8) * kCopyEff)));
        if (e < d)
            d = e, best = g;
    }
    return std::format("about Gen{} x8", best);
}
const double kGen5x8 = 0.95 * 3.938 * 8 * kCopyEff;

// Plain-language reasons multi-GPU runs slower than it could, most actionable first
std::vector<std::string> Hints(const Mgpu::Stats& st, bool full)
{
    std::vector<std::string> h;
    const Link r = LinkOf(st.render, st.sendGBs), d = LinkOf(st.display, st.ingestGBs);
    if (st.displayBusy > 80)
        h.push_back(full ? std::format("display-card bound ({:.0f}% busy): try a lower preset", st.displayBusy)
                         : "display-card bound, try a lower preset");
    // the timed copy is the frame alone; the HUD-less colour and vectors go over the same link
    const double sendAll = st.sendGBs > 0 ? st.frameBytes / (st.sendGBs * 1e6) : st.sendMs;
    const double sendShare = st.renderFrameMs > 0 && sendAll > 0 ? sendAll / st.renderFrameMs : 0;
    const bool rSlow = r.gbs > 0 && r.gbs < kGen5x8, dSlow = d.gbs > 0 && d.gbs < kGen5x8;
    if (rSlow || dSlow || sendShare > 0.15)
    {
        const bool onDisplay = dSlow && !rSlow;
        const Link& l = onDisplay ? d : r;
        std::string slot = l.known ? std::format("PCIe Gen{} x{} slot", l.gen, l.lanes)
                                   : std::format("slot at {:.1f} GB/s ({})", l.measured, ClassOf(l.measured));
        if (!rSlow && !dSlow)
            // a full-speed slot: the volume is the cost
            h.push_back(full ? std::format("link-bound: sending a frame ({:.0f} MB with HUD-less colour and vectors) "
                                           "takes {:.2f} ms, {:.0f}% of a frame, on the render card's {}",
                                           st.frameBytes / 1e6, sendAll, sendShare * 100, slot)
                             : std::format("link-bound: send {:.0f}% of a frame ({:.0f} MB)", sendShare * 100,
                                           st.frameBytes / 1e6));
        else if (full)
            h.push_back(std::format("link-bound: the {} card is in a {}; sending a frame ({:.0f} MB with HUD-less colour and "
                                    "vectors) takes {:.2f} ms, {:.0f}% of a frame (Gen5 x8: about {:.2f} ms)",
                                    onDisplay ? "display" : "render", slot, st.frameBytes / 1e6, sendAll,
                                    sendShare * 100, st.frameBytes / (Mgpu::LinkGBs(5, 8) * kCopyEff * 1e6)));
        else
            h.push_back(std::format("link-bound: {} card in a {}", onDisplay ? "display" : "render", slot));
    }
    if (st.renderBusy >= 99)
        h.push_back(full ? "render-card bound: the game's own rendering is the limit, not multi-GPU"
                         : "render-card bound");
    return h;
}
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
    s.overlap = std::clamp(g_keys.Int("MgpuOverlap", 0), 0, 2);
    s.lowLatency = std::clamp(g_keys.Int("MgpuLowLatency", 2), 0, 2);
    s.earlySend = g_keys.Bool("MgpuEarlySend", false);
    s.presentIdle = g_keys.Bool("MgpuPresentIdle", false);
    // MgpuDred: true / 1 breadcrumbs + page faults, 2 page faults only (breadcrumbs serialise the queue)
    s.dred = std::clamp(g_keys.Int("MgpuDred", g_keys.Bool("MgpuDred", false) ? 1 : 0), 0, 2);
    s.forwardOutputs = g_keys.Bool("MgpuForwardOutputs", true);

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

void MgpuOnPresent(OptiPresentFrame* f)
{
    if (!g_on)
        return;
    g_colorSpace = f->colorSpace;
    if (f->phase == OptiPresent_AfterOverlay && f->hudless != nullptr && Mgpu::WantHudless())
        CaptureHudless(f);
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
    line(std::format("Display card: {} (bus {}), {} busy", st.display.name, st.display.bus, Busy(st.displayBusy)));
    auto linkLine = [&](const char* who, const Mgpu::AdapterView& a, const char* copy, double ms, double gbs)
    {
        const Link l = LinkOf(a, gbs);
        std::string t = l.known ? std::format("{} link: PCIe Gen{} x{} ({:.1f} GB/s raw)", who, l.gen, l.lanes,
                                              Mgpu::LinkGBs(l.gen, l.lanes))
                                : std::format("{} link: not reported by the driver", who);
        if (gbs > 0)
            t += std::format(", {} {:.2f} ms at {:.1f} GB/s", copy, ms, gbs) +
                 (l.known ? std::format(" ({:.0f}% of raw)", 100 * gbs / Mgpu::LinkGBs(l.gen, l.lanes))
                          : std::format(" ({})", ClassOf(gbs)));
        line(t);
    };
    linkLine("Render card", st.render, "send", st.sendMs, st.sendGBs);
    linkLine("Display card", st.display, "ingest", st.ingestMs, st.ingestGBs);
    line(std::format("Mode: {}, NR every frame {}, motion vectors {}, frames ahead {}", s.hudless ? "HUD-less" : "whole frame",
                     s.nrEveryFrame ? "on" : "off", s.temporal ? "on" : "off", s.maxLead));
    line(std::format("Frame {}x{}, render {:.2f} ms, display {:.2f} ms", st.width, st.height, st.renderFrameMs,
                     st.displayFrameMs));
    if (st.addedMs >= 0)
        line(std::format("Added latency {:.1f} ms: render card done -> frame on the display card {:.1f} ms, "
                         "display card (network, UI, present) {:.1f} ms",
                         st.addedMs, st.renderToDisplayMs, st.displayToPresentMs));
    else
        line("Added latency: measuring");
    api->TextDisabled(std::format("(game Present -> real Present {:.1f} ms: includes queueing a single card has too)",
                                  st.latencyMs)
                          .c_str());
    for (const auto& h : Hints(st, true))
        api->TextColored(1.0f, 0.8f, 0.3f, 1.0f, h.c_str());
    line(std::format("Send {:.2f} ms ({:.1f} GB/s), ingest {:.2f} ms ({:.1f} GB/s); per frame {:.1f} MB with the "
                     "HUD-less colour and vectors, about {:.2f} ms on the render card's link",
                     st.sendMs, st.sendGBs, st.ingestMs, st.ingestGBs, st.frameBytes / 1e6,
                     st.sendGBs > 0 ? st.frameBytes / (st.sendGBs * 1e6) : 0.0));
    line(std::format("CPU waits: send {:.2f} ms, network answer {:.2f} ms", st.sendWaitMs, st.answerWaitMs));
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
             Busy(st.displayBusy).c_str(), st.addedMs >= 0 ? st.addedMs : st.latencyMs, nr.hipMs);
    return true;
}

// Second overlay line: both links and why multi-GPU is slower, if it is
bool MgpuOverlayHint(char* buf, size_t size)
{
    if (!g_on || !Mgpu::Redirected())
        return false;
    const auto st = Mgpu::GetStats();
    if (st.fallback)
        return false;
    const Link r = LinkOf(st.render, st.sendGBs), d = LinkOf(st.display, st.ingestGBs);
    std::string t = std::format("PCIe R {} {:.1f} GB/s, D {} {:.1f} GB/s", r.Short(), st.sendGBs, d.Short(), st.ingestGBs);
    for (const auto& h : Hints(st, false))
        t += "; " + h;
    snprintf(buf, size, "%s", t.c_str());
    // what the overlay shows, for runs nobody looks at
    static auto last = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(10))
    {
        last = std::chrono::steady_clock::now();
        char l1[256] {};
        MgpuOverlayText(l1, sizeof(l1));
        Log(OptiLog_Info, "[mgpu] overlay: MGPU: %s | Link: %s", l1, buf);
        for (const auto& h : Hints(st, true))
            Log(OptiLog_Info, "[mgpu] menu hint: %s", h.c_str());
    }
    return true;
}
