// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
#include "Runtime.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace
{
bool IsFile(const fs::path& p)
{
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool IsDir(const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory(p, ec);
}

// lmxxf's package: weights at native-game-tiled-assets, HIP modules + SHA256SUMS in its HIP folder
bool FindPackageAssets(const fs::path& dir, std::wstring* assets)
{
    for (const auto& base : { dir / L"DLSS5-AMD" / L"native-game-tiled-assets", dir / L"native-game-tiled-assets" })
    {
        if (IsFile(base / L"HIP" / L"SHA256SUMS"))
        {
            *assets = (base / L"HIP").wstring();
            return true;
        }
        if (IsFile(base / L"SHA256SUMS"))
        {
            *assets = base.wstring();
            return true;
        }
    }
    return false;
}

// lmxxf's header: LmxxfNrGetApi is the sole export. A runtime with more is a modified build whose appended table
// entries may not be lmxxf's, so it only gets the ABI v1 table.
size_t CountExports(HMODULE module)
{
    auto base = reinterpret_cast<const BYTE*>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0)
        return 0;
    return reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress)->NumberOfNames;
}

bool BinaryHas(const std::vector<char>& image, const char* text)
{
    const std::string_view needle(text);
    return std::search(image.begin(), image.end(), needle.begin(), needle.end()) != image.end();
}
} // namespace

bool Runtime::Load(const std::wstring& extraDir, const std::wstring& exeDir, const std::wstring& optiDir)
{
    std::vector<fs::path> dirs;
    if (!extraDir.empty())
        dirs.emplace_back(extraDir);
    for (const auto& d : { exeDir, optiDir })
    {
        if (!d.empty())
        {
            dirs.emplace_back(d);
            dirs.emplace_back(fs::path(d) / L"DLSS5-AMD");
        }
    }

    for (const auto& dir : dirs)
    {
        const auto dll = dir / L"LmxxfNrRuntime.dll";
        if (!IsFile(dll))
            continue;

        // Assets: lmxxf's package layout first, then a pak beside the dll
        std::wstring found;
        Layout foundLayout = Layout::None;
        for (const auto& root : { dir, dir.parent_path(), fs::path(exeDir) })
        {
            if (FindPackageAssets(root, &found))
            {
                foundLayout = Layout::LmxxfPackage;
                break;
            }
        }
        if (foundLayout == Layout::None && IsFile(dir / L"LmxxfNrRuntime.pak"))
        {
            found = (dir / L"LmxxfNrRuntime.pak").wstring();
            foundLayout = Layout::RuntimePak;
        }
        if (foundLayout == Layout::None)
        {
            error = "LmxxfNrRuntime.dll found but no assets (DLSS5-AMD\\native-game-tiled-assets or LmxxfNrRuntime.pak)";
            continue;
        }

        module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr)
        {
            error = "LoadLibrary(LmxxfNrRuntime.dll) failed: " + std::to_string(GetLastError());
            continue;
        }

        auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(module, "LmxxfNrGetApi"));
        if (getApi == nullptr)
        {
            error = "LmxxfNrRuntime.dll has no LmxxfNrGetApi export";
            FreeLibrary(module);
            module = nullptr;
            continue;
        }

        // The full table (with GetTimings) first, then the ABI v1 table
        exports = CountExports(module);
        int32_t rc = LMXXF_NR_INVALID_ARGUMENT;
        if (exports == 1)
        {
            api = {};
            api.struct_size = sizeof(LmxxfNrApi);
            rc = getApi(LMXXF_NR_ABI_VERSION, &api);
        }
        hasTimings = rc == LMXXF_NR_OK && api.struct_size == sizeof(LmxxfNrApi) && api.GetTimings != nullptr;
        if (rc != LMXXF_NR_OK)
        {
            api = {};
            api.struct_size = LMXXF_NR_API_V1_SIZE;
            rc = getApi(LMXXF_NR_ABI_VERSION, &api);
            tableSize = LMXXF_NR_API_V1_SIZE;
        }
        else
        {
            tableSize = sizeof(LmxxfNrApi);
        }
        // A modified runtime that takes only its own, larger table: the smallest size it accepts. Only the ABI v1
        // prefix is used through api; the rest stays in table for capability checked extensions.
        for (uint32_t size = sizeof(LmxxfNrApi); rc != LMXXF_NR_OK && size <= sizeof(table); size += 8)
        {
            memset(table, 0, sizeof(table));
            *reinterpret_cast<uint32_t*>(table) = size;
            rc = getApi(LMXXF_NR_ABI_VERSION, reinterpret_cast<LmxxfNrApi*>(table));
            if (rc == LMXXF_NR_OK)
            {
                api = {};
                memcpy(&api, table, LMXXF_NR_API_V1_SIZE);
                api.struct_size = LMXXF_NR_API_V1_SIZE;
                tableSize = size;
            }
        }
        if (rc != LMXXF_NR_OK || api.Create == nullptr || api.PrepareFrame == nullptr || api.EnqueueHip == nullptr)
        {
            error = "LmxxfNrGetApi failed: " + std::to_string(rc) + " " + LastError();
            FreeLibrary(module);
            module = nullptr;
            continue;
        }

        caps = {};
        caps.struct_size = sizeof(caps);
        if (api.QueryCapabilities != nullptr)
            api.QueryCapabilities(&caps);

        std::ifstream f(dll, std::ios::binary);
        const std::vector<char> image((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        envFreeRes = BinaryHas(image, "DLSS5_NETWORK_FREE_RES");
        envStyle = BinaryHas(image, "DLSS5_STYLE");
        envMultiPass = BinaryHas(image, "DLSS5_MULTI_PASS");

        dllPath = dll.wstring();
        assets = found;
        layout = foundLayout;
        error.clear();
        return true;
    }

    if (error.empty())
        error = "LmxxfNrRuntime.dll not found (game folder, OptiScaler folder or DLSS5-AMD)";
    return false;
}

std::string Runtime::LastError() const
{
    char buf[1024] = {};
    if (api.GetLastError != nullptr)
        api.GetLastError(buf, sizeof(buf));
    return buf;
}

const char* Runtime::LayoutName() const
{
    switch (layout)
    {
    case Layout::LmxxfPackage:
        return "lmxxf package";
    case Layout::RuntimePak:
        return "runtime + pak";
    default:
        return "none";
    }
}

void SetRuntimeEnv(const char* name, const char* value)
{
    SetEnvironmentVariableA(name, value);

    // getenv reads the CRT's own copy: update the ones the runtime may be linked against
    using PutEnv = int(__cdecl*)(const char*, const char*);
    for (const wchar_t* crt : { L"ucrtbase.dll", L"msvcrt.dll" })
    {
        HMODULE m = GetModuleHandleW(crt);
        if (m == nullptr)
            m = LoadLibraryW(crt);
        if (m == nullptr)
            continue;
        if (auto put = reinterpret_cast<PutEnv>(GetProcAddress(m, "_putenv_s")))
            put(name, value);
    }
}
