// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
// Hot-reload loader for development: installed as lmxxf_nr.asi, it runs the plugin from
// <OptiScaler folder>\lmxxf_nr_hot\<version>\lmxxf_nr.dll (the newest version folder by name) and swaps it for
// another version on <OptiScaler folder>\lmxxf_nr_dumps\lmxxf_nr_reload.trigger (a version name inside, or empty
// for the newest). The swap runs at a present, between frames: the plugin's callbacks are held, OptiPluginShutdown
// waits for the GPU and removes its detour, then the module is unloaded and the new one initialised with the same
// settings. Single-GPU only: with multi-GPU the plugin owns the game's device and swapchain and is never unloaded.
#include <windows.h>

#include "../../../src/plugin_sdk/optiscaler_plugin.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace
{
const OptiPluginApi* g_host = nullptr;
OptiPlugin* g_plugin = nullptr;
OptiPluginApi g_api {}; // what the plugin gets: the host's table with the registrations redirected here
SRWLOCK g_lock = SRWLOCK_INIT;
HMODULE g_core = nullptr;
std::wstring g_coreVersion;
bool g_pinned = false; // an adapter override or swapchain provider was registered: never unload
std::wstring g_opti;

struct
{
    OptiUpscaleFn before = nullptr, after = nullptr;
    void* user = nullptr;
    bool registered = false;
} g_up;
struct
{
    OptiPresentFn fn = nullptr;
    void* user = nullptr;
    uint32_t requests = 0;
    bool registered = false;
} g_present;
constexpr int kSlots = 8;
struct Tab
{
    std::string name;
    OptiDrawTabFn fn = nullptr;
    void* user = nullptr;
} g_tabs[kSlots];
struct Overlay
{
    std::string label;
    OptiOverlayTextFn fn = nullptr;
    void* user = nullptr;
} g_overlays[kSlots];

void Log(OptiLogLevel level, const char* fmt, ...)
{
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    g_host->Log(g_plugin, level, buf);
}

struct Shared
{
    Shared() { AcquireSRWLockShared(&g_lock); }
    ~Shared() { ReleaseSRWLockShared(&g_lock); }
};

void T_Before(OptiUpscaleFrame* f, void*)
{
    Shared s;
    if (g_up.before)
        g_up.before(f, g_up.user);
}

void T_After(OptiUpscaleFrame* f, void*)
{
    Shared s;
    if (g_up.after)
        g_up.after(f, g_up.user);
}

void T_Tab(void* slot)
{
    Shared s;
    const Tab& t = g_tabs[(intptr_t) slot];
    if (t.fn)
        t.fn(t.user);
    else
        g_host->TextDisabled("Plugin module not loaded (hot reload)");
    if (g_core)
    {
        char v[160];
        snprintf(v, sizeof(v), "Hot-reload loader: module %ls%s", g_coreVersion.c_str(), g_pinned ? " (multi-GPU: pinned)" : "");
        g_host->TextDisabled(v);
    }
}

bool T_Overlay(char* buf, size_t size, void* slot)
{
    Shared s;
    const Overlay& o = g_overlays[(intptr_t) slot];
    return o.fn ? o.fn(buf, size, o.user) : false;
}

bool W_RegisterTab(OptiPlugin* p, const char* name, const char* desc, OptiDrawTabFn fn, void* user)
{
    for (intptr_t i = 0; i < kSlots; i++)
        if (g_tabs[i].name == name)
            return g_tabs[i].fn = fn, g_tabs[i].user = user, true;
    for (intptr_t i = 0; i < kSlots; i++)
        if (g_tabs[i].name.empty())
        {
            g_tabs[i] = { name, fn, user };
            return g_host->RegisterTab(p, name, desc, T_Tab, (void*) i);
        }
    return false;
}

bool W_RegisterOverlayText(OptiPlugin* p, const char* label, OptiOverlayTextFn fn, void* user)
{
    for (intptr_t i = 0; i < kSlots; i++)
        if (g_overlays[i].label == label)
            return g_overlays[i].fn = fn, g_overlays[i].user = user, true;
    for (intptr_t i = 0; i < kSlots; i++)
        if (g_overlays[i].label.empty())
        {
            g_overlays[i] = { label, fn, user };
            return g_host->RegisterOverlayText(p, label, T_Overlay, (void*) i);
        }
    return false;
}

bool W_RegisterUpscaleCallbacks(OptiPlugin* p, OptiUpscaleFn before, OptiUpscaleFn after, void* user)
{
    g_up.before = before;
    g_up.after = after;
    g_up.user = user;
    if (!g_up.registered)
        g_up.registered = g_host->RegisterUpscaleCallbacks(p, T_Before, T_After, nullptr);
    return g_up.registered;
}

void Poll();

void T_Present(OptiPresentFrame* f, void*)
{
    {
        Shared s;
        if (g_present.fn)
            g_present.fn(f, g_present.user);
    }
    if (f->phase == OptiPresent_AfterOverlay)
        Poll();
}

bool W_RegisterPresentCallback(OptiPlugin* p, OptiPresentFn fn, uint32_t requests, void* user)
{
    g_present.fn = fn;
    g_present.user = user;
    if (!g_present.registered)
    {
        g_present.requests = requests;
        g_present.registered = g_host->RegisterPresentCallback(p, T_Present, requests, nullptr);
    }
    else if (requests != g_present.requests)
        Log(OptiLog_Warn, "hot reload: present requests %u -> %u need a restart", g_present.requests, requests);
    return g_present.registered;
}

bool W_RegisterAdapterOverride(OptiPlugin* p, OptiAdapterOverrideFn fn, void* user)
{
    g_pinned = true;
    return g_host->RegisterAdapterOverride(p, fn, user);
}

bool W_RegisterSwapchainProvider(OptiPlugin* p, OptiSwapchainProviderFn fn, void* user)
{
    g_pinned = true;
    return g_host->RegisterSwapchainProvider(p, fn, user);
}

std::wstring NewestVersion()
{
    std::wstring best;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(g_opti + L"\\lmxxf_nr_hot", ec))
        if (e.is_directory() && std::filesystem::exists(e.path() / L"lmxxf_nr.dll") && e.path().filename().wstring() > best)
            best = e.path().filename().wstring();
    return best;
}

// Under the exclusive lock (or in OptiPluginInit)
bool LoadCore(const std::wstring& version)
{
    const std::wstring path = g_opti + L"\\lmxxf_nr_hot\\" + version + L"\\lmxxf_nr.dll";
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m == nullptr)
        return Log(OptiLog_Error, "hot reload: can't load %ls (%lu)", path.c_str(), GetLastError()), false;
    auto init = (PFN_OptiPluginInit) GetProcAddress(m, OPTI_PLUGIN_INIT_NAME);
    g_core = m;
    g_coreVersion = version;
    if (init == nullptr || !init(&g_api, g_plugin))
    {
        Log(OptiLog_Error, "hot reload: %ls did not initialise", path.c_str());
        g_up.before = g_up.after = nullptr;
        g_present.fn = nullptr;
        for (auto& t : g_tabs)
            t.fn = nullptr;
        for (auto& o : g_overlays)
            o.fn = nullptr;
        return false;
    }
    Log(OptiLog_Info, "hot reload: module %ls loaded", version.c_str());
    return true;
}

void Reload(const std::wstring& version)
{
    if (g_pinned)
        return Log(OptiLog_Warn, "hot reload refused: multi-GPU keeps the module (restart the game)");
    const std::wstring to = version.empty() ? NewestVersion() : version;
    if (to.empty())
        return Log(OptiLog_Error, "hot reload: no version in lmxxf_nr_hot");
    AcquireSRWLockExclusive(&g_lock);
    using Shutdown = bool (*)();
    auto shutdown = g_core ? (Shutdown) GetProcAddress(g_core, "OptiPluginShutdown") : nullptr;
    if (g_core && (shutdown == nullptr || !shutdown()))
    {
        ReleaseSRWLockExclusive(&g_lock);
        return Log(OptiLog_Warn, "hot reload: module %ls refused to shut down, kept", g_coreVersion.c_str());
    }
    g_up.before = g_up.after = nullptr;
    g_present.fn = nullptr;
    for (auto& t : g_tabs)
        t.fn = nullptr;
    for (auto& o : g_overlays)
        o.fn = nullptr;
    if (g_core)
        FreeLibrary(g_core);
    const std::wstring from = g_coreVersion;
    g_core = nullptr;
    const DWORD t0 = GetTickCount();
    const bool ok = LoadCore(to);
    ReleaseSRWLockExclusive(&g_lock);
    Log(ok ? OptiLog_Info : OptiLog_Error, "hot reload %ls -> %ls %s in %lu ms", from.c_str(), to.c_str(),
        ok ? "done" : "FAILED", GetTickCount() - t0);
}

void Poll()
{
    static uint32_t n = 0;
    if (n++ % 30 != 20)
        return;
    const std::wstring trig = g_opti + L"\\lmxxf_nr_dumps\\lmxxf_nr_reload.trigger";
    FILE* f = _wfopen(trig.c_str(), L"r");
    if (f == nullptr)
        return;
    char v[128] = {};
    if (fscanf(f, "%127s", v) != 1)
        v[0] = 0;
    fclose(f);
    DeleteFileW(trig.c_str());
    const std::string s = v;
    Reload(std::wstring(s.begin(), s.end()));
}
} // namespace

extern "C" __declspec(dllexport) bool OptiPluginInit(const OptiPluginApi* api, OptiPlugin* plugin)
{
    if (api == nullptr || api->version < 1)
        return false;
    g_host = api;
    g_plugin = plugin;
    memcpy(&g_api, api, std::min<size_t>(api->size, sizeof(g_api)));
    g_api.size = (uint32_t) std::min<size_t>(api->size, sizeof(g_api));
    g_api.RegisterTab = W_RegisterTab;
    if (OPTI_API_HAS(api, RegisterUpscaleCallbacks))
        g_api.RegisterUpscaleCallbacks = W_RegisterUpscaleCallbacks;
    if (OPTI_API_HAS(api, RegisterOverlayText))
        g_api.RegisterOverlayText = W_RegisterOverlayText;
    if (OPTI_API_HAS(api, RegisterPresentCallback))
        g_api.RegisterPresentCallback = W_RegisterPresentCallback;
    if (OPTI_API_HAS(api, RegisterAdapterOverride))
        g_api.RegisterAdapterOverride = W_RegisterAdapterOverride;
    if (OPTI_API_HAS(api, RegisterSwapchainProvider))
        g_api.RegisterSwapchainProvider = W_RegisterSwapchainProvider;

    HMODULE opti = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) api->Log, &opti);
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(opti, p, MAX_PATH);
    g_opti = std::filesystem::path(p).parent_path().wstring();

    const std::wstring v = NewestVersion();
    if (v.empty())
        return Log(OptiLog_Error, "hot-reload loader: no %ls\\lmxxf_nr_hot\\<version>\\lmxxf_nr.dll", g_opti.c_str()), false;
    return LoadCore(v);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
