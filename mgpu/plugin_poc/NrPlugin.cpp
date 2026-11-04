// Proof of concept: an ASI plugin for stock OptiScaler that sees every DX12 upscaler evaluate.
// It detours OptiScaler's exported NVSDK_NGX_D3D12_EvaluateFeature, which the DLSS, FSR 2/3, FFX API
// and XeSS input paths all reach, logs the inputs and shows them in a menu tab. No NR runs yet.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include <nvsdk_ngx.h>

#include <detours/detours.h>

#include "optiscaler_plugin.h"

namespace
{
const OptiPluginApi* g_api = nullptr;
OptiPlugin* g_plugin = nullptr;

using PFN_Evaluate = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
PFN_Evaluate o_Evaluate = nullptr;

struct ResInfo
{
    bool present = false;
    UINT64 width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct FrameInfo
{
    ResInfo color, output, depth, mv, exposure;
    float jitterX = 0, jitterY = 0, mvScaleX = 0, mvScaleY = 0;
    int reset = 0;
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    double cpuMs = 0;
};

std::mutex g_mutex;
FrameInfo g_last;
std::atomic<uint64_t> g_evaluates { 0 };
std::atomic<uint64_t> g_failed { 0 };

int32_t g_logEvery = 300;
int32_t g_placement = 1; // 0 pre, 1 post, 2 terminal: recorded only, nothing runs
bool g_logInputs = true;
const char* const kPlacements[] = { "before upscaling", "after upscaling", "terminal (HUD-less)" };

void Log(OptiLogLevel level, const char* fmt, ...)
{
    if (g_api == nullptr)
        return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_api->Log(g_plugin, level, buf);
}

ResInfo Describe(NVSDK_NGX_Parameter* params, const char* name)
{
    ResInfo info;
    ID3D12Resource* res = nullptr;
    if (params->Get(name, &res) != NVSDK_NGX_Result_Success || res == nullptr)
        params->Get(name, (void**) &res);
    if (res == nullptr)
        return info;
    auto desc = res->GetDesc();
    info.present = true;
    info.width = desc.Width;
    info.height = desc.Height;
    info.format = desc.Format;
    return info;
}

std::string Format(const char* name, const ResInfo& r)
{
    char buf[96];
    if (r.present)
        snprintf(buf, sizeof(buf), "%s=%llux%u fmt%d", name, r.width, r.height, (int) r.format);
    else
        snprintf(buf, sizeof(buf), "%s=none", name);
    return buf;
}

NVSDK_NGX_Result __cdecl hkEvaluate(ID3D12GraphicsCommandList* cmdList, const NVSDK_NGX_Handle* handle,
                                    NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback)
{
    FrameInfo f;
    if (params != nullptr)
    {
        f.color = Describe(params, NVSDK_NGX_Parameter_Color);
        f.output = Describe(params, NVSDK_NGX_Parameter_Output);
        f.depth = Describe(params, NVSDK_NGX_Parameter_Depth);
        f.mv = Describe(params, NVSDK_NGX_Parameter_MotionVectors);
        f.exposure = Describe(params, NVSDK_NGX_Parameter_ExposureTexture);
        params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &f.jitterX);
        params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &f.jitterY);
        params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &f.mvScaleX);
        params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &f.mvScaleY);
        params->Get(NVSDK_NGX_Parameter_Reset, &f.reset);
    }

    // The NR pass would be recorded here (pre) or after the call (post), into cmdList
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    f.result = o_Evaluate(cmdList, handle, params, callback);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    f.cpuMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);

    const uint64_t n = ++g_evaluates;
    if (NVSDK_NGX_FAILED(f.result))
        g_failed++;

    if (g_logInputs && (n == 1 || (g_logEvery > 0 && n % (uint64_t) g_logEvery == 0)))
    {
        Log(OptiLog_Info, "evaluate #%llu %s %s %s %s %s jitter=%.3f,%.3f mvscale=%.1f,%.1f reset=%d result=0x%X cpu=%.3fms",
            n, Format("color", f.color).c_str(), Format("output", f.output).c_str(), Format("depth", f.depth).c_str(),
            Format("mv", f.mv).c_str(), Format("exposure", f.exposure).c_str(), f.jitterX, f.jitterY, f.mvScaleX,
            f.mvScaleY, f.reset, (unsigned) f.result, f.cpuMs);
    }

    std::lock_guard lock(g_mutex);
    g_last = f;
    return f.result;
}

bool InstallHook()
{
    // The api table lives in OptiScaler's image, whatever the dll is named (dxgi.dll, winmm.dll, ...)
    HMODULE opti = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR) g_api, &opti))
    {
        Log(OptiLog_Error, "can't find the OptiScaler module");
        return false;
    }

    o_Evaluate = (PFN_Evaluate) GetProcAddress(opti, "NVSDK_NGX_D3D12_EvaluateFeature");
    if (o_Evaluate == nullptr)
    {
        Log(OptiLog_Error, "OptiScaler has no NVSDK_NGX_D3D12_EvaluateFeature export");
        return false;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_Evaluate, hkEvaluate);
    const LONG err = DetourTransactionCommit();
    if (err != NO_ERROR)
    {
        Log(OptiLog_Error, "detour of NVSDK_NGX_D3D12_EvaluateFeature failed: %ld", err);
        o_Evaluate = nullptr;
        return false;
    }

    Log(OptiLog_Info, "hooked NVSDK_NGX_D3D12_EvaluateFeature (DX12 upscaler evaluates)");
    return true;
}

void Text(const char* fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_api->Text(buf);
}

void DrawTab(void*)
{
    FrameInfo f;
    {
        std::lock_guard lock(g_mutex);
        f = g_last;
    }

    g_api->Section("Neural Rendering");
    if (g_api->Combo("Placement", &g_placement, kPlacements, 3, nullptr))
        g_api->SetInt(g_plugin, "Placement", g_placement);
    g_api->ItemTooltip("Recorded only. The proof of concept runs no NR pass.");
    if (g_api->Checkbox("Log inputs", &g_logInputs))
        g_api->SetBool(g_plugin, "LogInputs", g_logInputs);
    if (g_api->SliderInt("Log every N evaluates", &g_logEvery, 1, 3600, nullptr) && g_api->ItemDeactivatedAfterEdit())
        g_api->SetInt(g_plugin, "LogEvery", g_logEvery);

    g_api->Section("Stats");
    Text("Evaluates: %llu (failed %llu)", (unsigned long long) g_evaluates.load(), (unsigned long long) g_failed.load());
    Text("Upscaler CPU time: %.3f ms", f.cpuMs);
    Text("%s", Format("Color", f.color).c_str());
    Text("%s", Format("Output", f.output).c_str());
    Text("%s", Format("Depth", f.depth).c_str());
    Text("%s", Format("MVs", f.mv).c_str());
    Text("%s", Format("Exposure", f.exposure).c_str());
    Text("Jitter %.3f, %.3f  MV scale %.1f, %.1f", f.jitterX, f.jitterY, f.mvScaleX, f.mvScaleY);
    if (o_Evaluate == nullptr)
        g_api->TextColored(1.0f, 0.4f, 0.4f, 1.0f, "Evaluate hook not installed, see the log.");
    g_api->TextDisabled("Overlay stat: the plugin API has no overlay hook yet (see PLUGIN_API_ASSESSMENT.md).");

    if (g_api->Button("Save"))
        g_api->SaveSettings(g_plugin);
}
} // namespace

extern "C" __declspec(dllexport) bool OptiPluginInit(const OptiPluginApi* api, OptiPlugin* plugin)
{
    if (api == nullptr || api->version < 1)
        return false;

    g_api = api;
    g_plugin = plugin;

    g_placement = api->GetInt(plugin, "Placement", 1);
    if (g_placement < 0 || g_placement > 2)
        g_placement = 1;
    g_logEvery = api->GetInt(plugin, "LogEvery", 300);
    g_logInputs = api->GetBool(plugin, "LogInputs", true);

    Log(OptiLog_Info, "NrPlugin PoC loaded, plugin API v%u (size %u), placement=%s", api->version, api->size,
        kPlacements[g_placement]);

    InstallHook();
    return api->RegisterTab(plugin, "NR Plugin", "Neural Rendering plugin (proof of concept)", DrawTab, nullptr);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
