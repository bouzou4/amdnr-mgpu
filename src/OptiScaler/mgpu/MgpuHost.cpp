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
    return s;
}

// Whole-frame NR on the display card: AMDNR's final image context, on the display device.
static AmdFinalImage::Context* s_nr = nullptr;
static bool s_nrFailed = false;

static bool NrFrame(ID3D12Device* d, ID3D12CommandQueue* q, ID3D12Resource* target)
{
    std::lock_guard g(AmdFinalImage::mutex);
    if (s_nrFailed)
        return false;
    try
    {
        if (!s_nr)
        {
            s_nr = new AmdFinalImage::Context(d, q);
            LOG_INFO("[MGPU] NR context on the display card");
        }
        return s_nr->FrameFrom(target, D3D12_RESOURCE_STATE_PRESENT, target, D3D12_RESOURCE_STATE_PRESENT,
                               Util::DllPath().parent_path());
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
                           IDXGIFactory* f = nullptr;
                           if (!DxgiProxy::CreateDxgiFactory_() ||
                               DxgiProxy::CreateDxgiFactory_()(__uuidof(IDXGIFactory1), &f) != S_OK)
                               return nullptr;
                           return (IDXGIFactory1*) f;
                       };
                       e.noSpoof = [](const std::function<void()>& fn)
                       {
                           ScopedSkipSpoofingThread s {};
                           fn();
                       };
                       e.nrFrame = NrFrame;
                       e.nrWanted = NrWanted;
                       e.nrStatus = [] { return AmdFinalImage::Status(); };
                       Mgpu::Configure(ReadSettings(), e);
                   });
}

bool InternalDeviceCreate() { return Mgpu::InternalCreate(); }

IUnknown* AdapterForDevice(IUnknown* adapter)
{
    EnsureConfigured();
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
                           "HUD-less: NR on the HUD-less colour, the HUD composited back (not built yet: runs whole "
                           "frame).\n[Mgpu] Mode");

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
