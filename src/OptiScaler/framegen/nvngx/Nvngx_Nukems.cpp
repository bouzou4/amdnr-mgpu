// Modifications Copyright (c) 2026 3zwr1 (AMDNR)
#include "pch.h"
#include "Nvngx_Nukems.h"

void Nvngx_Nukems::LoadLibraries()
{
    HMODULE memModule = nullptr;
    auto& optiPath = Config::Instance()->MainDllPath.value();
    // The zip ships Nukem9's release binary renamed amdnr_dlssg_fsr3.dll (0.3.2); a
    // dlssg_to_fsr3_amd_is_better.dll the player put there themselves is honoured too.
    Util::LoadProxyLibrary(L"amdnr_dlssg_fsr3.dll", L"", optiPath, &memModule, &dll);
    if (dll == nullptr && memModule == nullptr)
        Util::LoadProxyLibrary(L"dlssg_to_fsr3_amd_is_better.dll", L"", optiPath, &memModule, &dll);

    if (dll == nullptr && memModule != nullptr)
        dll = memModule;

    if (dll != nullptr)
    {
        _refreshGlobalConfiguration =
            (PFN_RefreshGlobalConfiguration) GetProcAddress(dll, "RefreshGlobalConfiguration");

        LoadNgxExports();

        LOG_INFO("Nukem's initialized");
    }
    else
    {
        LOG_WARN("Nukem's enabled but cannot be loaded");
    }
}

void Nvngx_Nukems::setSetting(const wchar_t* setting, const wchar_t* value)
{
    if (is120orNewer())
    {
        SetEnvironmentVariable(setting, value);
        _refreshGlobalConfiguration();
    }
}

void Nvngx_Nukems::setDebugView(bool enabled)
{
    auto setting = L"DLSSGTOFSR3_EnableDebugOverlay";
    auto value = enabled ? L"1" : L"";
    setSetting(setting, value);
}

void Nvngx_Nukems::setInterpolatedOnly(bool enabled)
{
    auto setting = L"DLSSGTOFSR3_EnableInterpolatedFramesOnly";
    auto value = enabled ? L"1" : L"";
    setSetting(setting, value);
}
