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

bool ResolveRuntime(const Autoconfig::RuntimeCandidate& c, const std::wstring& exeDir, RuntimeLayout* out)
{
    using L = Runtime::Layout;
    *out = {};
    out->weights = c.weights;
    for (const fs::path& dir : { fs::path(c.dir), fs::path(c.dir) / L"DLSS5-AMD" })
    {
        const auto dll = dir / L"LmxxfNrRuntime.dll";
        if (!IsFile(dll))
            continue;

        // Assets: ModulesDir, lmxxf's package layout, a module folder beside the dll, then a pak beside the dll
        std::wstring found;
        L foundLayout = L::None;
        if (!c.modules.empty() && IsFile(fs::path(c.modules) / L"SHA256SUMS"))
        {
            found = c.modules;
            foundLayout = L::LmxxfBuild;
        }
        for (const auto& root : { dir, dir.parent_path(), fs::path(exeDir) })
        {
            if (foundLayout != L::None)
                break;
            if (FindPackageAssets(root, &found))
                foundLayout = L::LmxxfPackage;
        }
        for (const auto& sub : { dir / L"modules", dir / L"HIP" })
        {
            if (foundLayout == L::None && IsFile(sub / L"SHA256SUMS"))
            {
                found = sub.wstring();
                foundLayout = L::LmxxfBuild;
            }
        }
        if (foundLayout == L::None && IsFile(dir / L"LmxxfNrRuntime.pak"))
        {
            found = (dir / L"LmxxfNrRuntime.pak").wstring();
            foundLayout = L::RuntimePak;
        }
        if (foundLayout == L::None)
        {
            out->error = "LmxxfNrRuntime.dll found but no assets (ModulesDir, DLSS5-AMD\\native-game-tiled-assets, "
                         "modules\\ or LmxxfNrRuntime.pak)";
            continue;
        }
        // Module folders carry SHA256SUMS: every listed module must hash right
        if (foundLayout != L::RuntimePak)
        {
            std::string why;
            if (!Autoconfig::VerifySha256Sums((fs::path(found) / L"SHA256SUMS").wstring(), &why, &out->hashed))
            {
                out->error = "modules " + Autoconfig::Narrow(found) + ": " + why;
                continue;
            }
        }
        out->dll = dll.wstring();
        out->assets = found;
        out->layout = (int) foundLayout;
        out->error.clear();
        return true;
    }
    if (out->error.empty())
        out->error = "no LmxxfNrRuntime.dll";
    return false;
}

bool Runtime::Load(const RuntimePaths& paths)
{
    tried = true;
    // The runtime polls GetAsyncKeyState: F9 cycles DLSS5_MULTI_PASS and rewrites custom-config.txt, F8 the ViT reuse.
    // Games bind those keys (HL2: F9 quick load); the plugin owns these settings. Set before load: env beats the files.
    SetRuntimeEnv("DLSS5_MULTI_PASS_HOTKEY", "0");
    SetRuntimeEnv("DLSS5_VIT_REUSE_HOTKEY", "0");
    searched.clear();
    for (const auto& c : paths.candidates)
    {
        RuntimeLayout r;
        if (!ResolveRuntime(c, paths.exe, &r))
        {
            searched.push_back(c.source + ": " + Autoconfig::Narrow(c.dir) + ": " + r.error);
            continue;
        }
        const fs::path dll(r.dll);
        const Layout foundLayout = (Layout) r.layout;
        const std::wstring found = r.assets;
        hashed = r.hashed;

        // Read by the runtime when the session finds no weights beside its modules
        if (!r.weights.empty() && foundLayout != Layout::RuntimePak)
            SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", r.weights.c_str());

        module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr)
        {
            error = "LoadLibrary(LmxxfNrRuntime.dll) failed: " + std::to_string(GetLastError());
            searched.push_back(c.source + ": " + error);
            continue;
        }

        auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(module, "LmxxfNrGetApi"));
        if (getApi == nullptr)
        {
            error = "LmxxfNrRuntime.dll has no LmxxfNrGetApi export";
            searched.push_back(c.source + ": " + error);
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
            searched.push_back(c.source + ": " + error);
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
        source = c.source;
        error.clear();
        return true;
    }

    error = "no usable lmxxf runtime found";
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
    case Layout::LmxxfBuild:
        return "lmxxf build + modules";
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
