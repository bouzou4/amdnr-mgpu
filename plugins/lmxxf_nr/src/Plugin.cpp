// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
// OptiScaler ASI plugin: lmxxf's DLSS 5 Neural Rendering runtime around the upscaler.
#include "MgpuPlugin.h"
#include "Nr.h"
#include "Presets.h"
#include "mgpu/MgpuCore.h"

#include <detours/detours.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>

const OptiPluginApi* g_api = nullptr;
OptiPlugin* g_plugin = nullptr;

namespace
{
NrSettings g_ui; // what the tab edits, applied with Apply
bool g_needsApi = false;
bool g_hasPresent = false;

void ConfigureAll(const NrSettings& s)
{
    Engine().Configure(s);
    DisplayEngine().Configure(s);
}
bool g_hudlessRequested = false;

const char* const kPlacements[] = { "Before upscaling", "After upscaling", "Terminal (HUD-less)" };
const char* const kSizes[] = { "1080 tier", "1440 (2560x1440)", "Native" };
const char* const kModels[] = { "Full (71 blocks)", "Small (31 blocks)" };
const char* const kStyles[] = { "0 (NVIDIA look)", "1", "2" };
const char* const kPasses[] = { "1", "2 (about 2x the network)", "3 (2 run + 1 predicted, about 2x)" };

std::atomic<int> g_eclInFlight { 0 }; // calls inside the detour, waited out before a hot-reload unload

void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    g_eclInFlight++;
    Engine().OnExecute(queue, count, lists);
    g_eclInFlight--;
}

NrSettings ReadSettings()
{
    NrSettings s;
    s.enabled = g_api->GetBool(g_plugin, "Enabled", true);
    s.placement = (Placement) std::clamp(g_api->GetInt(g_plugin, "Placement", 1), 0, 2);
    s.size = (NetworkSize) std::clamp(g_api->GetInt(g_plugin, "NetworkSize", 0), 0, 2);
    s.model = std::clamp(g_api->GetInt(g_plugin, "Model", 0), 0, 1);
    s.passes = std::clamp(g_api->GetInt(g_plugin, "MultiPass", 1), 1, 3);
    s.style = std::clamp(g_api->GetInt(g_plugin, "Style", 0), 0, 2);
    s.strength = std::clamp(g_api->GetFloat(g_plugin, "Strength", 1.0f), 0.0f, 2.0f);
    s.motion = g_api->GetBool(g_plugin, "MotionVectors", true);
    s.exposure = g_api->GetBool(g_plugin, "Exposure", true);
    s.serialize = g_api->GetBool(g_plugin, "Serialize", false);
    s.temporal = g_api->GetBool(g_plugin, "Temporal", true);
    s.paperWhite = std::clamp(g_api->GetFloat(g_plugin, "HdrPaperWhite", 200.0f), 80.0f, 1000.0f);
    s.wholeFrameFallback = g_api->GetBool(g_plugin, "WholeFrameFallback", true);
    s.meter = g_api->GetBool(g_plugin, "FlickerMeter", true);
    return s;
}

void WriteSettings(const NrSettings& s)
{
    g_api->SetBool(g_plugin, "Enabled", s.enabled);
    g_api->SetInt(g_plugin, "Placement", (int32_t) s.placement);
    g_api->SetInt(g_plugin, "NetworkSize", (int32_t) s.size);
    g_api->SetInt(g_plugin, "Model", s.model);
    g_api->SetInt(g_plugin, "MultiPass", s.passes);
    g_api->SetInt(g_plugin, "Style", s.style);
    g_api->SetFloat(g_plugin, "Strength", s.strength);
    g_api->SetBool(g_plugin, "MotionVectors", s.motion);
    g_api->SetBool(g_plugin, "Exposure", s.exposure);
    g_api->SetBool(g_plugin, "Temporal", s.temporal);
    g_api->SetBool(g_plugin, "WholeFrameFallback", s.wholeFrameFallback);
    g_api->SetInt(g_plugin, "Preset", (int32_t) MatchPreset(s));
}

void Text(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_api->Text(buf);
}

RuntimePaths g_paths;

// Not from OptiPluginInit: that runs inside OptiScaler's DllMain
void EnsureRuntime()
{
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        Engine().LoadRuntime(g_paths);
    }
}

void UpscaleBefore(OptiUpscaleFrame* frame, void*)
{
    if (MgpuActive())
        return; // NR runs on the display card
    EnsureRuntime();
    Engine().OnUpscale(frame, false);
}

void UpscaleAfter(OptiUpscaleFrame* frame, void*)
{
    if (MgpuActive())
        return MgpuOnUpscale(frame);
    EnsureRuntime();
    Engine().OnUpscale(frame, true);
    if (Engine().Settings().placement == Placement::Terminal)
        Engine().CaptureMotion(frame);
}

// Game present cadence, logged every 10 s in every mode (PresentMon may see nothing)
void FramePacing(const OptiPresentFrame* frame)
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last {}, windowStart {};
    static std::vector<float> ms;
    const auto now = Clock::now();
    if (last.time_since_epoch().count())
        ms.push_back(std::chrono::duration<float, std::milli>(now - last).count());
    else
        windowStart = now;
    last = now;
    if (now - windowStart < std::chrono::seconds(10) || ms.empty())
        return;
    double sum = 0;
    for (float v : ms)
        sum += v;
    std::sort(ms.begin(), ms.end());
    Log(OptiLog_Info, "pacing: %zu presents, mean %.2f ms, median %.2f, p99 %.2f, max %.2f, %ux%u fmt %u cs %u",
        ms.size(), sum / ms.size(), ms[ms.size() / 2], ms[std::min(ms.size() - 1, ms.size() * 99 / 100)], ms.back(),
        frame->width, frame->height, frame->format, frame->colorSpace);
    ms.clear();
    windowStart = now;
}

// Test rigs: <dumpDir>\\lmxxf_nr_settings.trigger, Key=Value lines (Preset, Model, NetworkSize, MultiPass, Temporal,
// Style, FlickerMeter), applied live like the menu
std::wstring g_dumpDir;
void SettingsTrigger()
{
    static uint32_t n = 0;
    if (g_dumpDir.empty() || n++ % 30 != 0)
        return;
    const std::wstring path = g_dumpDir + L"\\lmxxf_nr_settings.trigger";
    FILE* f = _wfopen(path.c_str(), L"r");
    if (f == nullptr)
        return;
    char line[128];
    std::string applied;
    while (fgets(line, sizeof(line), f))
    {
        char key[64] = {};
        int v = 0;
        if (sscanf(line, " %63[^=]=%d", key, &v) != 2)
            continue;
        const std::string k = key;
        if (k == "Preset")
            ApplyPreset(g_ui, PresetFromIni(v));
        else if (k == "Model")
            g_ui.model = std::clamp(v, 0, 1);
        else if (k == "NetworkSize")
            g_ui.size = (NetworkSize) std::clamp(v, 0, 2);
        else if (k == "MultiPass")
            g_ui.passes = std::clamp(v, 1, 3);
        else if (k == "Temporal")
            g_ui.temporal = v != 0;
        else if (k == "Style")
            g_ui.style = std::clamp(v, 0, 2);
        else if (k == "FlickerMeter")
            g_ui.meter = v != 0;
        else
            continue;
        applied += (applied.empty() ? "" : ", ") + k + "=" + std::to_string(v);
    }
    fclose(f);
    DeleteFileW(path.c_str());
    ConfigureAll(g_ui);
    Log(OptiLog_Info, "settings trigger: %s", applied.c_str());
}

// Keys read only at start: the ini trigger sets them in memory, they apply after a restart
bool RestartOnly(const std::string& k)
{
    return k == "MultiGpu" || k.rfind("Mgpu", 0) == 0 || k == "RuntimeDir" || k == "ModulesDir" || k == "WeightsDir" ||
           (k == "Placement" && !g_hudlessRequested);
}

// Test rigs: <dumpDir>\lmxxf_nr_ini.trigger, Key=Value lines of [Plugin.lmxxf_nr] (any key): set in memory, then
// every setting is read again and applied like the menu
void IniTrigger()
{
    static uint32_t n = 0;
    if (g_dumpDir.empty() || n++ % 30 != 15)
        return;
    const std::wstring path = g_dumpDir + L"\\lmxxf_nr_ini.trigger";
    FILE* f = _wfopen(path.c_str(), L"r");
    if (f == nullptr)
        return;
    auto trim = [](const std::string& v)
    {
        const size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    };
    char line[600];
    std::string applied, restart;
    bool preset = false;
    while (fgets(line, sizeof(line), f))
    {
        const std::string l = line;
        const size_t eq = l.find('=');
        if (l[0] == ';' || l[0] == '#' || eq == std::string::npos)
            continue;
        const std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        if (k.empty())
            continue;
        g_api->SetString(g_plugin, k.c_str(), v.c_str());
        preset |= k == "Preset";
        std::string& to = RestartOnly(k) ? restart : applied;
        to += (to.empty() ? "" : ", ") + k + "=" + v;
    }
    fclose(f);
    DeleteFileW(path.c_str());
    g_ui = ReadSettings();
    if (const Preset p = PresetFromIni(g_api->GetInt(g_plugin, "Preset", 0)); preset && p != Preset::Custom)
        ApplyPreset(g_ui, p);
    ConfigureAll(g_ui);
    Log(OptiLog_Info, "ini trigger: %s%s%s", applied.c_str(), restart.empty() ? "" : "; restart-only: ", restart.c_str());
}

// Test rigs: <dumpDir>\lmxxf_nr_frames.trigger "<seconds> <tag>": present-to-present ms for that long, one per
// line, into frames_<tag>.txt
void FrameLogTrigger()
{
    using Clock = std::chrono::steady_clock;
    static uint32_t n = 0;
    static std::vector<float> ms;
    static Clock::time_point start {}, last {};
    static double seconds = 0;
    static std::string tag;
    const auto now = Clock::now();
    if (seconds > 0)
    {
        ms.push_back(std::chrono::duration<float, std::milli>(now - last).count());
        last = now;
        if (now - start < std::chrono::duration<double>(seconds))
            return;
        const std::wstring out = g_dumpDir + L"\\frames_" + std::wstring(tag.begin(), tag.end()) + L".txt";
        if (FILE* o = _wfopen((out + L".tmp").c_str(), L"w"))
        {
            for (float v : ms)
                fprintf(o, "%.3f\n", v);
            fclose(o);
            MoveFileExW((out + L".tmp").c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        Log(OptiLog_Info, "frame log %s: %zu presents in %.1f s", tag.c_str(), ms.size(), seconds);
        seconds = 0;
        ms.clear();
        return;
    }
    if (g_dumpDir.empty() || n++ % 10 != 5)
        return;
    const std::wstring path = g_dumpDir + L"\\lmxxf_nr_frames.trigger";
    FILE* f = _wfopen(path.c_str(), L"r");
    if (f == nullptr)
        return;
    double s = 10;
    char t[64] = "w";
    if (fscanf(f, "%lf %63s", &s, t) < 1)
        s = 10;
    fclose(f);
    DeleteFileW(path.c_str());
    seconds = std::clamp(s, 0.5, 600.0);
    tag = t;
    start = last = now;
}

void Present(OptiPresentFrame* frame, void*)
{
    if (frame->phase == OptiPresent_AfterOverlay)
    {
        FramePacing(frame);
        SettingsTrigger();
        IniTrigger();
        FrameLogTrigger();
    }
    if (MgpuActive())
        return MgpuOnPresent(frame);
    EnsureRuntime();
    Engine().OnPresent(frame);
}

bool MgpuText(char* buf, size_t size, void*) { return MgpuOverlayText(buf, size); }

void DrawTab(void*)
{
    NrEngine& nr = MgpuActive() ? DisplayEngine() : Engine();
    auto& rt = nr.GetRuntime();
    const NrStats st = nr.Stats();
    const NrSettings active = Engine().Settings();

    g_api->Section("Neural Rendering");
    bool changed = false;

    // Combo order: the presets, then Custom
    const char* presetNames[kPresetCount + 1];
    bool presetOff[kPresetCount + 1] = {};
    const PresetCaps caps { rt.FreeRes(), rt.Models() };
    std::string tip;
    for (int32_t i = 0; i < kPresetCount; ++i)
    {
        const PresetRows& r = kPresets[i];
        presetNames[i] = r.name;
        const char* why = rt.module != nullptr ? PresetBlocked(r.id, caps) : nullptr;
        presetOff[i] = why != nullptr;
        tip += std::string(r.name) + ": " + (r.model == 1 ? "Small" : "Full") + ", " + kSizes[(int) r.size] + ", " +
               std::to_string(r.passes) + (r.passes == 1 ? " pass" : " passes") +
               (r.temporal && r.passes == 1 ? ", history on" : ", history off (more than one pass)") +
               ".\n    One GPU: " + r.cost + (why ? std::string(".\n    Unavailable: ") + why : std::string()) + ".\n";
    }
    presetNames[kPresetCount] = "Custom";
    tip += "Custom: the rows below as set by hand.\n"
           "Multi-GPU: the display card runs the network, so the cost is hidden while it has headroom\n"
           "(busy % beside the presets). 1080 tier about 8 ms there (measured), Quality about 11 ms and\n"
           "Ultra about 25 ms (estimated): Ultra at 4K likely exceeds a 60 fps frame even there.\n"
           "A preset only sets Model, Network size, Passes and history; changing one of them shows Custom.";
    const Preset current = MatchPreset(g_ui);
    int32_t presetIdx = kPresetCount;
    for (int32_t i = 0; i < kPresetCount; ++i)
        if (kPresets[i].id == current)
            presetIdx = i;
    if (g_api->Combo("Preset", &presetIdx, presetNames, kPresetCount + 1, presetOff) && presetIdx < kPresetCount)
        ApplyPreset(g_ui, kPresets[presetIdx].id), changed = true;
    g_api->ItemTooltip(tip.c_str());
    if (MgpuActive())
    {
        const double busy = Mgpu::GetStats().displayBusy;
        g_api->SameLine();
        if (busy >= 0)
            Text("display card %.0f%% busy", busy);
        else
            g_api->TextDisabled("display card busy: -");
    }

    changed |= g_api->Checkbox("Enabled", &g_ui.enabled);

    int32_t placement = (int32_t) g_ui.placement;
    const bool placementOff[] = { false, false, !g_hasPresent };
    if (g_api->Combo("Placement", &placement, kPlacements, 3, placementOff))
        g_ui.placement = (Placement) placement, changed = true;
    g_api->ItemTooltip("Before upscaling: NR on the render-size colour the upscaler gets.\n"
                       "After upscaling: NR on the upscaler's output, before post-processing and UI.\n"
                       "Terminal: NR on the HUD-less frame at present, the UI kept as drawn (needs a restart when\n"
                       "it was not the placement at start).");
    if (g_ui.placement == Placement::Terminal && !g_hudlessRequested)
        g_api->TextColored(1.0f, 0.6f, 0.3f, 1.0f, "Terminal takes effect after a restart (Save first).");
    if (g_ui.placement == Placement::Terminal)
    {
        if (g_api->Checkbox("Whole frame without a HUD-less colour", &g_ui.wholeFrameFallback))
            changed = true;
        g_api->ItemTooltip("No HUD-less colour (none found, or the game's own frame generation is on): NR on the\n"
                           "whole frame, UI included, on every present. Off: terminal NR waits for a HUD-less colour.");
    }

    int32_t size = (int32_t) g_ui.size;
    const bool sizeOff[] = { false, !rt.FreeRes(), !rt.FreeRes() };
    if (g_api->Combo("Network size", &size, kSizes, 3, sizeOff))
        g_ui.size = (NetworkSize) size, changed = true;
    g_api->ItemTooltip(rt.FreeRes() ? "1080 tier: the picture is fitted into 1920x1080.\n1440 and Native: "
                                      "DLSS5_NETWORK_FREE_RES, cost 1.7x / up to 3.6x the 1080 tier."
                                    : "This runtime has fixed tiers only (no DLSS5_NETWORK_FREE_RES).");

    g_api->Disabled(!rt.Models());
    if (g_api->Combo("Model", &g_ui.model, kModels, 2, nullptr))
        changed = true;
    g_api->Disabled(false);
    g_api->ItemTooltip(rt.Models() ? "Full: the 71-block model, Small: 31 blocks (DLSS5_MULTI_PASS 1 / 0.5)"
                                   : "This runtime has one model (no DLSS5_MULTI_PASS)");

    g_api->Disabled(!rt.Models());
    int32_t pass = g_ui.passes - 1;
    if (g_api->Combo("Passes", &pass, kPasses, 3, nullptr))
        g_ui.passes = pass + 1, changed = true;
    g_api->Disabled(false);
    g_api->ItemTooltip(!rt.Models() ? "This runtime has one model (no DLSS5_MULTI_PASS)"
                                    : "DLSS5_MULTI_PASS: the Full network runs N times per frame for a stronger style.\n"
                                      "Full model only: the runtime runs Small once (DLSS5_MULTI_PASS 0.5).\n"
                                      "3 predicts its third pass unless DLSS5_MULTI_PASS_PREDICT=0 is set. 2 and 3 run\n"
                                      "without network history (the runtime's history takes one pass).");
    if (rt.Models() && g_ui.model != 0 && g_ui.passes > 1)
        g_api->TextColored(1.0f, 0.6f, 0.3f, 1.0f, "Passes apply to the Full model: pick Full to use them.");

    g_api->Disabled(!rt.Styles());
    if (g_api->Combo("Style", &g_ui.style, kStyles, 3, nullptr))
        changed = true;
    g_api->Disabled(false);
    g_api->ItemTooltip(rt.Styles() ? "DLSS5_STYLE, 0 matches NVIDIA's output" : "This runtime does not read DLSS5_STYLE");

    if (g_api->SliderFloat("Strength", &g_ui.strength, 0.0f, 2.0f, "%.2f"))
        changed = true;
    if (g_api->Checkbox("Move the edit with motion vectors", &g_ui.motion))
        changed = true;
    if (g_api->Checkbox("Network history from motion vectors", &g_ui.temporal))
        changed = true;
    g_api->ItemTooltip("Runtimes with motion-fed history (lmxxf temporal builds): the network sees its last answer");
    if (g_api->Checkbox("Game exposure", &g_ui.exposure))
        changed = true;
    g_api->ItemTooltip("Before/after upscaling with HDR colour: map it into the network's range with the game's\n"
                       "exposure texture (off or missing: auto-exposure). SDR: give the runtime the texture.");

    if (changed)
        ConfigureAll(g_ui);
    if (g_api->Button("Save"))
    {
        WriteSettings(g_ui);
        g_api->SaveSettings(g_plugin);
    }
    g_api->TextDisabled("Size, model, passes and style rebuild the network (a few seconds without NR).");

    g_api->Section("Runtime");
    if (rt.module == nullptr)
    {
        g_api->TextColored(1.0f, 0.45f, 0.4f, 1.0f, rt.error.empty() ? "Loaded at the first upscaled frame" : rt.error.c_str());
    }
    else
    {
        Text("%ls", rt.dllPath.c_str());
        Text("Layout: %s, %s function table, frame info %u bytes%s", rt.LayoutName(), rt.hasTimings ? "full" : "v1",
             rt.temporalSize ? rt.temporalSize : st.frameInfoSize, rt.Native() ? ", lmxxf history ABI" : "");
        Text("Assets: %ls", rt.assets.c_str());
    }

    g_api->Section("Multi-GPU");
    MgpuDrawTab(g_api);

    g_api->Section("Stats");
    Text("State: %s", st.state.c_str());
    if (!st.error.empty())
        g_api->TextColored(1.0f, 0.6f, 0.3f, 1.0f, st.error.c_str());
    Text("Source %ux%u -> network %ux%u", st.srcWidth, st.srcHeight, st.feedWidth, st.feedHeight);
    Text("Network + handoff (GPU): %.2f ms", st.hipMs);
    if (rt.hasTimings)
        Text("Network only (runtime): %.2f ms", st.networkMs);
    static const char* const kEnc[] = { "linear", "SDR (sRGB)", "HDR10 (PQ)", "HDR scRGB", "scene HDR" };
    const char* enc = kEnc[std::min<uint32_t>(st.encoding, 4)];
    if (st.encoding == 4)
        Text("Colour: %s, the network gets exposure-mapped values (%s exposure, x%.3g, shoulder above 0.8)", enc,
             st.gameExposure ? "game" : "auto", st.netScale);
    else if (st.encoding >= 2)
        Text("Colour: %s, the network gets tonemapped values (paper white %.0f nits, HdrPaperWhite)", enc,
             active.paperWhite);
    else
        Text("Colour: %s", enc);
    if (st.bypassed)
        g_api->TextColored(1.0f, 0.6f, 0.3f, 1.0f, "NR bypassed: fed median above 1.5 after mapping (colour range not understood)");
    if (!st.source.empty() && active.placement == Placement::Terminal)
        Text("Source: %s", st.source.c_str());
    Text("Network history: %s", st.temporal ? "on (motion vectors)" : rt.temporal == 0 ? "not taken by this runtime" : "off");
    if (st.flkFrames > 0)
    {
        Text("Flicker: still %+.4f, moving %s (last %u frames)", st.flkStatic,
             std::isnan(st.flkMc) ? "-" : std::format("{:+.4f}", st.flkMc).c_str(), st.flkFrames);
        g_api->ItemTooltip("How much more the network's answer changes from frame to frame than its input does\n"
                           "(tools/seq_metrics.py flk_static / flk_mc, display encoding). Above 0: NR adds flicker,\n"
                           "below 0: it steadies the image. Moving needs this frame's answer (multi-GPU, NR every frame).");
    }
    Text("Frames: %llu run, %llu skipped", (unsigned long long) st.framesRun, (unsigned long long) st.framesSkipped);
    if (!st.status.empty())
        g_api->TextDisabled(st.status.c_str());
    if (!g_needsApi)
        g_api->TextColored(1.0f, 0.45f, 0.4f, 1.0f, "This OptiScaler has no upscale callbacks: NR can't run.");
    (void) active;
}

bool OverlayText(char* buf, size_t size, void*)
{
    const NrSettings s = Engine().Settings();
    if (!s.enabled || !Engine().RuntimeLoaded())
        return false;
    if (MgpuFallback())
    {
        snprintf(buf, size, "off (display card lost)");
        return true;
    }
    NrEngine& nr = MgpuActive() ? DisplayEngine() : Engine();
    const NrStats st = nr.Stats();
    const char* fulls[] = { " Full", " Full x2", " Full x3" };
    const char* model = nr.GetRuntime().Models() ? (s.model == 1 ? " Small" : fulls[std::clamp(s.passes, 1, 3) - 1]) : "";
    if (st.state != "running")
        snprintf(buf, size, "%s", st.state.c_str());
    else if (st.bypassed)
        snprintf(buf, size, "bypassed (HDR range)");
    else if (st.networkMs > 0)
        snprintf(buf, size, "%ux%u%s%s%s, %.1f ms (network %.1f)%s", st.feedWidth, st.feedHeight, model,
                 st.temporal ? " T" : "", st.encoding >= 2 ? " HDR" : "", st.hipMs, st.networkMs,
                 st.flkFrames ? std::format(", flk {:+.4f}", std::isnan(st.flkMc) ? st.flkStatic : st.flkMc).c_str() : "");
    else
        snprintf(buf, size, "%ux%u%s%s%s, %.1f ms", st.feedWidth, st.feedHeight, model, st.temporal ? " T" : "",
                 st.encoding >= 2 ? " HDR" : "", st.hipMs);
    return true;
}

std::wstring ModuleDir(HMODULE m)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().wstring();
}
} // namespace

bool InstallQueueHook(ID3D12Device* device)
{
    if (o_ExecuteCommandLists != nullptr)
        return true;
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueue> probe;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&probe))))
        return false;
    // ID3D12CommandQueue::ExecuteCommandLists, vtable slot 10
    auto vtable = *reinterpret_cast<void***>(probe.Get());
    o_ExecuteCommandLists = reinterpret_cast<PFN_ExecuteCommandLists>(vtable[10]);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);
    const LONG err = DetourTransactionCommit();
    if (err != NO_ERROR)
    {
        Log(OptiLog_Error, "ExecuteCommandLists detour failed: %ld", err);
        return false;
    }
    Log(OptiLog_Info, "hooked ID3D12CommandQueue::ExecuteCommandLists");
    return true;
}

extern "C" __declspec(dllexport) bool OptiPluginInit(const OptiPluginApi* api, OptiPlugin* plugin)
{
    if (api == nullptr || api->version < 1)
        return false;
    g_api = api;
    g_plugin = plugin;

    g_ui = ReadSettings();
    if (const Preset p = PresetFromIni(g_api->GetInt(g_plugin, "Preset", 0)); p != Preset::Custom)
    {
        ApplyPreset(g_ui, p);
        Log(OptiLog_Info, "preset %s: model %s, size %s, passes %d, history %s", FindPreset(p)->name,
            kModels[g_ui.model], kSizes[(int) g_ui.size], g_ui.passes, g_ui.temporal ? "on" : "off");
    }
    ConfigureAll(g_ui);

    // The OptiScaler module holds the api table; its folder is where OptiScaler.dll / dxgi.dll lives
    HMODULE opti = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) api->Log, &opti); // not api: under the hot-reload loader it is the loader's copy
    auto path = [&](const char* key)
    {
        char v[MAX_PATH] = {};
        api->GetString(plugin, key, v, sizeof(v), "");
        return std::filesystem::path(v).wstring();
    };
    g_paths.extra = path("RuntimeDir");
    g_paths.modules = path("ModulesDir");
    g_paths.weights = path("WeightsDir");
    g_paths.exe = ModuleDir(nullptr);
    g_paths.opti = ModuleDir(opti);

    const std::wstring dumpDir = g_paths.opti + L"\\lmxxf_nr_dumps";
    Engine().dumpDir = dumpDir;
    g_dumpDir = dumpDir;
    const bool mgpu = MgpuInit(api, plugin, dumpDir);

    g_needsApi = OPTI_API_HAS(api, RegisterUpscaleCallbacks);
    if (g_needsApi)
        api->RegisterUpscaleCallbacks(plugin, UpscaleBefore, UpscaleAfter, nullptr);
    else
        Log(OptiLog_Error, "OptiScaler has no RegisterUpscaleCallbacks, NR can't run");
    g_hasPresent = OPTI_API_HAS(api, RegisterPresentCallback);
    if (g_hasPresent)
    {
        g_hudlessRequested = g_ui.placement == Placement::Terminal || (mgpu && MgpuWantsHudless());
        api->RegisterPresentCallback(plugin, Present, g_hudlessRequested ? OptiPresentRequest_Hudless : 0, nullptr);
    }
    if (OPTI_API_HAS(api, RegisterOverlayText))
    {
        api->RegisterOverlayText(plugin, "NR", OverlayText, nullptr);
        if (mgpu)
            api->RegisterOverlayText(plugin, "MGPU", MgpuText, nullptr);
    }

    Log(OptiLog_Info, "lmxxf_nr loaded, plugin API size %u, placement %s, size %s", api->size,
        kPlacements[(int) g_ui.placement], kSizes[(int) g_ui.size]);
    return api->RegisterTab(plugin, "Neural Rendering", "lmxxf DLSS 5 Neural Rendering", DrawTab, nullptr);
}

// Hot-reload loader: called at a present before this module is unloaded. Waits for the GPU, destroys the session,
// removes the queue detour. False keeps the module (multi-GPU owns the game's device and swapchain).
extern "C" __declspec(dllexport) bool OptiPluginShutdown()
{
    if (MgpuActive() || MgpuFallback() || !Engine().Shutdown())
        return false;
    if (o_ExecuteCommandLists != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);
        if (DetourTransactionCommit() != NO_ERROR)
            return Log(OptiLog_Error, "hot reload: detour removal failed"), false;
        for (int i = 0; i < 1000 && g_eclInFlight.load() != 0; i++)
            Sleep(1);
        Sleep(50); // a caller between the jump and the counter
        o_ExecuteCommandLists = nullptr;
    }
    delete &Engine(); // GPU idle: its D3D12 objects can go
    delete &DisplayEngine();
    Log(OptiLog_Info, "shut down for hot reload");
    return true;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}

void MgpuLoadRuntime(NrEngine& e) { e.LoadRuntime(g_paths); }
