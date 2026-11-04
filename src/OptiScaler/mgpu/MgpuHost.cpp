// Adapter between OptiScaler and the multi-GPU core: config, logging, NR backend, menu and overlay.
#include "pch.h"
#include "MgpuHost.h"
#include "MgpuCore.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <proxies/Dxgi_Proxy.h>
#include <dlssnr/amd/PresentExperimental.h>
#include <dlssnr/menu/NeuralUi.h>
#include <dlssnr/lmxxf/LmxxfBackend.h>
#include <hudfix/Hudfix_Dx12.h>
#include <upscalers/IFeature_Dx12.h>
#include <NVNGX_Parameter.h>
#include <imgui/imgui.h>

#include <mutex>
#include <format>

namespace MgpuHost
{
static std::once_flag s_once;

static Mgpu::Settings ReadSettings()
{
    auto* c = Config::Instance();
    Mgpu::Settings s;
    s.enabled = c->MgpuEnabled.value_or_default();
    s.renderBus = c->MgpuRenderAdapterBus.value_or_default();
    s.displayBus = c->MgpuDisplayAdapterBus.value_or_default();
    s.hudless = c->MgpuMode.value_or_default() == "hudless";
    s.presenter = c->MgpuPresenter.value_or_default();
    s.fallbackInline = c->MgpuFallback.value_or_default() != "off";
    s.nrEveryFrame = c->MgpuNrEveryFrame.value_or_default();
    s.temporal = c->MgpuTemporal.value_or_default();
    s.uiGate = std::clamp(c->MgpuUiGate.value_or_default(), 0.f, 100.f) / 100.0;
    return s;
}

// NR on the display card: AMDNR's final image context, on the display device. Without motion it runs the
// no-motion carry; with the render card's motion vectors ([Mgpu] Temporal) the backend's motion path, network
// history on, the post-upscale layout (display-sized colour, render-sized vectors, no jitter).
static AmdFinalImage::Context* s_nr = nullptr;
static bool s_nrFailed = false;

static bool TemporalFrame(AmdFinalImage::Context& c, const Mgpu::NrInput& in, const std::filesystem::path& dir)
{
    using AmdFinalImage::Transition;
    if (c.stopped)
        return false;
    const auto desc = in.src->GetDesc();
    const bool sdr8 = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM && !State::Instance().isHdrActive);
    if (!sdr8 && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return false;
    c.EnsureResources(desc);
    if (!c.backend)
    {
        if (!DlssNr::AmdBridge::LmxxfReady())
            return false;
        c.lmxxf = true;
        c.backend = new Lmxxf::Backend(c.device.Get(), c.queue.Get(), dir);
        LOG_INFO("[MGPU] NR temporal stage: lmxxf runtime, network history from the render card's motion vectors");
    }
    if (!c.backend->Ready() && c.backend->RecordedFrames() != 0)
        return false;
    auto& slot = c.ring[c.ringIndex];
    c.ringIndex = (c.ringIndex + 1) % AmdFinalImage::Context::kRing;
    c.WaitFor(slot.fenceValue);
    AmdFinalImage::Check(slot.allocator->Reset(), "allocator reset");
    AmdFinalImage::Check(slot.cmd->Reset(slot.allocator.Get(), nullptr), "list reset");
    ID3D12GraphicsCommandList* cmd = slot.cmd.Get();
    if (!c.guidesCleared)
        c.ClearGuides(cmd);
    Transition(cmd, in.src, in.srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyResource(c.input.Get(), in.src);
    Transition(cmd, in.src, D3D12_RESOURCE_STATE_COPY_SOURCE, in.srcState);
    Transition(cmd, c.input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    AmdPreSr::Frame f {};
    f.colour = c.input.Get();
    f.width = c.width;
    f.height = c.height;
    f.colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    f.depth = c.depth.Get(); // constant: depth does not cross the ring yet
    f.displayReferred = true;
    if (in.motion)
    {
        f.motion = in.motion;
        f.motionState = in.motionState;
        f.motionWidth = in.guides.motionW;
        f.motionHeight = in.guides.motionH;
        f.motionScaleX = in.guides.mvScaleX;
        f.motionScaleY = in.guides.mvScaleY;
        f.depthInverted = in.guides.depthInverted;
    }
    else
        f.motion = c.motion.Get(); // zero field, history dropped below
    // The colour is the upscaler's output after post: unjittered.
    f.jitterX = f.jitterY = 0.f;
    f.reset = in.reset || !in.motion;
    if (f.reset)
        c.backend->InvalidateHistory();

    AmdPreSr::Settings s = DlssNr::AmdBridge::CurrentSettings();
    s.lmxxfNoMotion = false;
    s.lmxxfHistory = true;
    s.rtgi.enabled = false;
    // the colour is fed as is, like the no-motion carry (an sRGB decode drove auto-exposure to x25)
    if (!Config::Instance()->AmdNrScale.has_value())
        s.modelScale = (std::min)(s.modelScale, 0.5f);
    ID3D12Resource* result = c.backend->Record(cmd, f, s);
    Transition(cmd, c.input.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    const bool composed = result != nullptr;
    if (composed)
        c.Compose(cmd, in.dst, in.dstState, result);
    AmdFinalImage::Check(cmd->Close(), "close");
    ID3D12CommandList* lists[] = { cmd };
    c.backend->Submitting(c.queue.Get(), 1, lists);
    c.queue->ExecuteCommandLists(1, lists);
    c.backend->Submitted(c.queue.Get(), 1, lists);
    slot.fenceValue = ++c.serial;
    AmdFinalImage::Check(c.queue->Signal(c.fence.Get(), c.serial), "signal");
    AmdFinalImage::status = std::string(composed ? "Multi-GPU NR (temporal) active: " : "Multi-GPU NR (temporal) waiting: ") +
                            c.backend->Status();
    return composed;
}

static bool NrFrame(ID3D12Device* d, ID3D12CommandQueue* q, const Mgpu::NrInput& in)
{
    std::lock_guard g(AmdFinalImage::mutex);
    if (s_nrFailed || !in.src || !in.dst)
        return false;
    try
    {
        if (!s_nr)
        {
            s_nr = new AmdFinalImage::Context(d, q);
            LOG_INFO("[MGPU] NR context on the display card");
        }
        const auto dir = Util::DllPath().parent_path();
        if (Mgpu::CurrentSettings().temporal)
            return TemporalFrame(*s_nr, in, dir);
        // NrEveryFrame: wait for the answer in flight so this frame consumes a fresh one
        if (in.waitAnswer && s_nr->lmxxf && s_nr->backend)
            static_cast<Lmxxf::Backend*>(s_nr->backend)->WaitAnswer(250);
        return s_nr->FrameFrom(in.src, in.srcState, in.dst, in.dstState, dir);
    }
    catch (const std::exception& e)
    {
        AmdFinalImage::status = std::string("Multi-GPU NR stopped: ") + e.what();
        LOG_ERROR("[MGPU] {}", AmdFinalImage::status);
        s_nrFailed = true;
        return false;
    }
}
static bool NrWanted()
{
    return Config::Instance()->DlssNrEnabled.value_or_default() && DlssNr::AmdBridge::LmxxfReady();
}

static void EnsureConfigured()
{
    std::call_once(s_once,
                   []
                   {
                       Mgpu::Env e;
                       e.log = [](int lvl, const std::string& s)
                       {
                           if (lvl >= 2)
                               LOG_ERROR("{}", s);
                           else if (lvl == 1)
                               LOG_WARN("{}", s);
                           else
                               LOG_INFO("{}", s);
                       };
                       e.createFactory = []() -> IDXGIFactory1*
                       {
                           DxgiProxy::Init();
                           IDXGIFactory1* f = nullptr;
                           HRESULT hr = E_FAIL;
                           if (auto fn = DxgiProxy::CreateDxgiFactory1_())
                               hr = fn(__uuidof(IDXGIFactory1), &f);
                           if (FAILED(hr) || !f)
                           {
                               // the system dxgi.dll directly
                               wchar_t sys[MAX_PATH];
                               GetSystemDirectoryW(sys, MAX_PATH);
                               HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\dxgi.dll").c_str());
                               using PFN = HRESULT(WINAPI*)(REFIID, void**);
                               auto fn = m ? (PFN) GetProcAddress(m, "CreateDXGIFactory1") : nullptr;
                               hr = fn ? fn(__uuidof(IDXGIFactory1), (void**) &f) : E_NOINTERFACE;
                           }
                           LOG_INFO("[MGPU] factory {:X} -> {:X}", (size_t) f, (uint32_t) hr);
                           return SUCCEEDED(hr) ? f : nullptr;
                       };
                       e.noSpoof = [](const std::function<void()>& fn)
                       {
                           ScopedSkipSpoofingThread s {};
                           fn();
                       };
                       e.nrFrame = NrFrame;
                       e.nrWanted = NrWanted;
                       e.nrStatus = [] { return AmdFinalImage::Status(); };
                       e.onPresent = [] { if (HudlessSink()) Hudfix_Dx12::PresentStart(); };
                       e.dumpDir = (Util::DllPath().parent_path() / L"mgpu_dumps").wstring();
                       Mgpu::Configure(ReadSettings(), e);
                   });
}

bool InternalDeviceCreate() { return Mgpu::InternalCreate(); }

// Devices DXGI, D3D12 or OptiScaler create for themselves stay where they were asked for.
static bool SystemCaller(void* caller)
{
    HMODULE m = nullptr;
    if (!caller || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR) caller, &m))
        return false;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) &SystemCaller, &self);
    if (m == self)
        return true;
    wchar_t p[MAX_PATH];
    if (!GetModuleFileNameW(m, p, MAX_PATH))
        return false;
    std::wstring n = std::filesystem::path(p).filename().wstring();
    for (auto& ch : n)
        ch = towlower(ch);
    return n == L"dxgi.dll" || n == L"d3d12.dll" || n == L"d3d12core.dll" || n == L"d3d11.dll" || n == L"dxcore.dll";
}

IUnknown* AdapterForDevice(IUnknown* adapter, void* caller)
{
    EnsureConfigured();
    if (SystemCaller(caller))
        return adapter;
    return Mgpu::RedirectAdapter(adapter);
}

bool IsRenderAdapter(const LUID& luid)
{
    if (!Mgpu::Redirected())
        return false;
    auto s = Mgpu::GetStats();
    return s.render.luid.LowPart == luid.LowPart && s.render.luid.HighPart == luid.HighPart;
}

bool OwnsSwapchains()
{
    EnsureConfigured();
    return Mgpu::CurrentSettings().enabled && Mgpu::CurrentSettings().presenter && Mgpu::Redirected();
}

HRESULT CreateSwapChainForHwnd(void* original, IDXGIFactory2* factory, IUnknown* queue, HWND hwnd,
                               const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                               IDXGIOutput* restrict, IDXGISwapChain1** out)
{
    if (!OwnsSwapchains() || !queue)
        return E_NOTIMPL;
    ID3D12CommandQueue* q = nullptr;
    if (queue->QueryInterface(IID_PPV_ARGS(&q)) != S_OK)
        return E_NOTIMPL;
    q->Release();
    return Mgpu::CreateSwapChainForHwnd((Mgpu::PFN_CreateSwapChainForHwnd) original, factory, q, hwnd, desc, fs,
                                        restrict, out);
}

bool BlockRenderCardNr()
{
    if (Mgpu::NrOnDisplay())
        return true;
    const auto& s = Mgpu::CurrentSettings();
    return s.enabled && !s.fallbackInline && !Mgpu::Redirected(); // Fallback=off
}

// ---- render-card inputs ------------------------------------------------------------------------------------

bool HudlessSink()
{
    return Mgpu::WantHudless() && State::Instance().currentFG == nullptr;
}

void HudlessCaptured(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES state)
{
    static bool said = false;
    if (!said)
    {
        said = true;
        auto d = res->GetDesc();
        LOG_INFO("[MGPU] HUD-less pick from HUDfix: {}x{} fmt {}", d.Width, d.Height, (int) d.Format);
    }
    Mgpu::CaptureHudless(cl, res, state);
}

void UpscaleEvaluated(ID3D12GraphicsCommandList* cl, NVSDK_NGX_Parameter* params, IFeature_Dx12* feature)
{
    if (!cl || !params || !feature)
        return;
    if (HudlessSink())
    {
        static bool warned = false;
        if (State::Instance().activeFgInput != FGInput::Upscaler && !warned)
        {
            warned = true;
            LOG_WARN("[MGPU] Mode=hudless needs [FrameGen] FGInput=upscaler (HUDfix's resource tracking): whole frame");
        }
        // arms HUDfix for this frame; FG would do this in its own UpscaleEnd
        Hudfix_Dx12::UpscaleEnd(feature->FrameCount(), State::Instance().lastFGFrameTime);
    }
    if (!Mgpu::WantGuides())
        return;
    Mgpu::Guides g;
    ID3D12Resource* mv = nullptr;
    if (params->Get(NVSDK_NGX_Parameter_MotionVectors, &mv) != NVSDK_NGX_Result_Success)
        params->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &mv);
    if (!mv)
        return;
    g.motion = mv;
    g.motionState = (D3D12_RESOURCE_STATES) Config::Instance()->MVResourceBarrier.value_or(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.renderW = feature->RenderWidth();
    g.renderH = feature->RenderHeight();
    g.motionW = feature->LowResMV() ? g.renderW : feature->DisplayWidth();
    g.motionH = feature->LowResMV() ? g.renderH : feature->DisplayHeight();
    params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &g.mvScaleX);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &g.mvScaleY);
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &g.jitterX);
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &g.jitterY);
    int reset = 0;
    params->Get(NVSDK_NGX_Parameter_Reset, &reset);
    g.reset = reset != 0;
    g.depthInverted = feature->DepthInverted();
    static bool said = false;
    if (!said)
    {
        said = true;
        auto d = mv->GetDesc();
        LOG_INFO("[MGPU] motion vectors {}x{} fmt {} active {}x{} scale {} {} render {}x{} lowres {}", d.Width, d.Height,
                 (int) d.Format, g.motionW, g.motionH, g.mvScaleX, g.mvScaleY, g.renderW, g.renderH,
                 feature->LowResMV());
    }
    Mgpu::CaptureGuides(cl, g);
}

// ---- UI ---------------------------------------------------------------------------------------------------

static std::string CardLabel(const Mgpu::AdapterView& a)
{
    if (a.bus < 0)
        return "-";
    return std::format("{} (bus {}, {} GB)", a.name, a.bus, a.vram >> 30);
}

static std::string Busy(double v) { return v < 0 ? std::string("-") : std::format("{:.0f}%", v); }

void DrawMenuTab(float scale)
{
    namespace UI = DlssNr::NeuralUi;
    EnsureConfigured();
    auto* cfg = Config::Instance();
    const auto st = Mgpu::GetStats();

    UI::SectionHeader("Multi-GPU");
    bool en = cfg->MgpuEnabled.value_or_default();
    if (UI::Checkbox("Render on the second GPU", &en))
        cfg->MgpuEnabled = en;
    UI::Legacy::HelpMarker("The game renders on the card with no monitors; the card with the monitors receives "
                           "each frame over PCIe, runs Neural Rendering on it and presents. Needs a restart.\n"
                           "[Mgpu] Enabled");

    // Card picks, by name and PCI bus; "auto" = the card with no outputs renders.
    static std::vector<Mgpu::AdapterView> cards;
    static bool scanned = false;
    if (!scanned)
    {
        scanned = true;
        if (st.topology)
            cards = { st.render, st.display };
    }
    auto busCombo = [&](const char* label, CustomOptional<int32_t>& opt, const char* autoText)
    {
        int bus = opt.value_or_default();
        std::string preview = autoText;
        for (auto& c : cards)
            if (c.bus == bus)
                preview = CardLabel(c);
        if (UI::BeginCombo(label, preview.c_str()))
        {
            if (ImGui::Selectable(autoText, bus < 0))
                opt = -1;
            for (auto& c : cards)
                if (ImGui::Selectable(CardLabel(c).c_str(), c.bus == bus))
                    opt = c.bus;
            ImGui::EndCombo();
        }
    };
    ImGui::BeginDisabled(!en);
    busCombo("Render card", cfg->MgpuRenderAdapterBus, "Auto (no monitors)");
    UI::Legacy::HelpMarker("[Mgpu] RenderAdapterBus. Needs a restart.");
    busCombo("Display card", cfg->MgpuDisplayAdapterBus, "Auto (has the monitors)");
    UI::Legacy::HelpMarker("[Mgpu] DisplayAdapterBus. Needs a restart.");

    const char* modes[] = { "Whole frame", "HUD-less" };
    int mode = cfg->MgpuMode.value_or_default() == "hudless" ? 1 : 0;
    if (UI::Combo("NR input", &mode, modes, 2))
        cfg->MgpuMode = std::string(mode ? "hudless" : "whole-frame");
    UI::Legacy::HelpMarker("Whole frame: NR runs on the presented frame, HUD included.\n"
                           "HUD-less: NR on HUDfix's HUD-less pick, the HUD composited back from the back buffer (needs "
                           "FGInput=upscaler, FG output off; over 15% differing pixels runs whole frame).\n[Mgpu] Mode");

    const char* fb[] = { "Inline NR", "Off" };
    int fbi = cfg->MgpuFallback.value_or_default() == "off" ? 1 : 0;
    if (UI::Combo("Without a second GPU", &fbi, fb, 2))
        cfg->MgpuFallback = std::string(fbi ? "off" : "inline");
    UI::Legacy::HelpMarker("What runs when no second card is found: NR on the one card, or nothing.\n[Mgpu] Fallback");
    ImGui::EndDisabled();

    UI::SectionHeader("Cards");
    if (!st.topology)
    {
        UI::DimTag(en ? "No second card found (or restart needed): single GPU." : "Off: single GPU.");
        return;
    }
    if (ImGui::BeginTable("##mgpu_cards", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH))
    {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Render card");
        ImGui::TableSetupColumn("Display card");
        ImGui::TableHeadersRow();
        auto row = [](const char* k, const std::string& a, const std::string& b)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(b.c_str());
        };
        row("Card", CardLabel(st.render), CardLabel(st.display));
        row("Role", st.redirected ? "renders the game" : "idle (not redirected)",
            st.presenter ? (st.nrOnDisplay ? "runs NR, presents" : "presents") : "monitors only");
        row("Engine use", Busy(st.renderBusy), Busy(st.displayBusy));
        row("Frame time", std::format("{:.2f} ms ({:.0f} fps)", st.renderFrameMs,
                                      st.renderFrameMs > 0 ? 1000.0 / st.renderFrameMs : 0.0),
            std::format("{:.2f} ms ({:.0f} fps)", st.displayFrameMs,
                        st.displayFrameMs > 0 ? 1000.0 / st.displayFrameMs : 0.0));
        row("Frame copy", std::format("{:.2f} ms, {:.1f} GB/s", st.sendMs, st.sendGBs),
            std::format("{:.2f} ms, {:.1f} GB/s", st.ingestMs, st.ingestGBs));
        ImGui::EndTable();
    }
    UI::SectionHeader("Pipeline");
    ImGui::Text("Frame %ux%u fmt %u, ring %u slots x %.1f MB, depth %u", st.width, st.height, st.format, st.ringSlots,
                st.slotBytes / 1048576.0, st.ringDepth);
    ImGui::Text("Frames sent %llu, presented %llu, skipped %llu", st.produced, st.presented, st.skipped);
    ImGui::Text("Added latency (send to present) %.2f ms", st.latencyMs);
    ImGui::Text("Present: flip model, tearing allowed, on the display card");
    if (cfg->MgpuMode.value_or_default() == "hudless")
        ImGui::Text("HUD-less frames %llu, missed %llu, UI %.2f%% of pixels, gate fallbacks %llu", st.hudlessFrames,
                    st.hudlessMissed, st.uiShare * 100, st.gateFallbacks);
    if (cfg->MgpuTemporal.value_or_default())
        ImGui::Text("Frames with motion vectors %llu", st.guideFrames);
    if (st.nrOnDisplay)
    {
        ImGui::Text("NR record+submit %.2f ms", st.nrMs);
        ImGui::TextWrapped("%s", st.nrStatus.c_str());
    }
}

void DrawOverlayLines(bool horizontal)
{
    if (!Mgpu::CurrentSettings().enabled)
        return;
    const auto st = Mgpu::GetStats();
    if (!st.presenter)
        return;
    auto line = [&](const std::string& s)
    {
        if (horizontal)
        {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(" | ");
            ImGui::SameLine(0.0f, 0.0f);
        }
        else
            ImGui::Spacing();
        ImGui::TextUnformatted(s.c_str());
    };
    line(std::format("MGPU render bus {} {:.1f} fps {} | display bus {} {:.1f} fps {}", st.render.bus,
                     st.renderFrameMs > 0 ? 1000.0 / st.renderFrameMs : 0.0, Busy(st.renderBusy), st.display.bus,
                     st.displayFrameMs > 0 ? 1000.0 / st.displayFrameMs : 0.0, Busy(st.displayBusy)));
    line(std::format("Copy {:.2f}+{:.2f} ms, latency {:.1f} ms, skipped {}{}", st.sendMs, st.ingestMs, st.latencyMs,
                     st.skipped, st.nrOnDisplay ? std::format(", NR {:.2f} ms", st.nrMs) : std::string()));
}
} // namespace MgpuHost
