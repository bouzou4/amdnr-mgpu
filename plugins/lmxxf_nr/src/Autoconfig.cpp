// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
#include "Autoconfig.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

Autoconfig::Keys g_keys;

namespace fs = std::filesystem;

namespace Autoconfig
{
namespace
{
std::string Trim(std::string s)
{
    const auto ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    const auto e = s.find_last_not_of(ws);
    s.erase(e == std::string::npos ? 0 : e + 1);
    return s;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

std::wstring LowerW(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t) std::towlower(c); });
    return s;
}

bool Exists(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

bool IsFile(const fs::path& p)
{
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool IsUnset(const std::string& v)
{
    const std::string t = Lower(Trim(v));
    return t.empty() || t == "auto";
}

bool ParseBool(const std::string& v, bool def)
{
    const std::string t = Lower(Trim(v));
    if (t == "true" || t == "1" || t == "yes" || t == "on")
        return true;
    if (t == "false" || t == "0" || t == "no" || t == "off")
        return false;
    return def;
}

// Profile values that are words for a bool key of this build
std::string ProfileValue(const std::string& key, const std::string& v)
{
    if (Lower(key) == "exposure")
    {
        const std::string t = Lower(Trim(v));
        if (t == "game")
            return "true";
        if (t == "auto")
            return "false";
    }
    return v;
}
} // namespace

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}

bool Ini::Less::operator()(const std::string& a, const std::string& b) const { return _stricmp(a.c_str(), b.c_str()) < 0; }

bool Ini::Load(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    Parse(ss.str());
    return true;
}

void Ini::Parse(const std::string& text)
{
    std::istringstream in(text);
    std::string line, current;
    bool first = true;
    while (std::getline(in, line))
    {
        if (first && line.size() >= 3 && (unsigned char) line[0] == 0xEF && (unsigned char) line[1] == 0xBB)
            line.erase(0, 3); // UTF-8 BOM
        first = false;
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[')
        {
            const auto e = line.find(']');
            current = Trim(line.substr(1, e == std::string::npos ? std::string::npos : e - 1));
            if (sections.find(current) == sections.end())
                order.push_back(current);
            sections[current];
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos || current.empty())
            continue;
        std::string value = line.substr(eq + 1);
        // Inline comment: " ;" after the value
        if (const auto c = value.find(" ;"); c != std::string::npos)
            value.erase(c);
        sections[current][Trim(line.substr(0, eq))] = Trim(value);
    }
}

const Ini::Section* Ini::Find(const std::string& section) const
{
    const auto it = sections.find(section);
    return it == sections.end() ? nullptr : &it->second;
}

bool MatchProfile(const Ini& ini, const std::wstring& exeName, const std::wstring& exeDir, Profile* out)
{
    const std::string exe = Narrow(exeName);
    const std::wstring dirLower = LowerW(exeDir);
    const Ini::Section* best = nullptr;
    std::string bestName;
    size_t bestLen = 0;
    bool bestHasPath = false;
    for (const auto& name : ini.order)
    {
        // "Satisfactory.exe" and "Satisfactory.exe|2" (a second section for another Path) both match the exe
        const std::string base = name.substr(0, name.find('|'));
        if (_stricmp(base.c_str(), exe.c_str()) != 0)
            continue;
        const Ini::Section& s = ini.sections.at(name);
        const auto p = s.find("Path");
        if (p != s.end() && !Trim(p->second).empty())
        {
            const std::wstring needle = LowerW(Widen(Trim(p->second)));
            if (dirLower.find(needle) == std::wstring::npos)
                continue;
            if (!bestHasPath || needle.size() > bestLen)
                best = &s, bestName = name, bestLen = needle.size(), bestHasPath = true;
        }
        else if (best == nullptr)
        {
            best = &s, bestName = name;
        }
    }
    if (best == nullptr)
        return false;
    out->section = bestName;
    out->keys = *best;
    const auto n = best->find("Name");
    out->name = n != best->end() ? n->second : bestName;
    return true;
}

bool ProfileKeyUsed(const std::string& key)
{
    static const char* const kUsed[] = { "Placement", "Preset", "MgpuPreset", "WholeFrameFallback", "Exposure",
                                         "HdrPaperWhite", "Model", "NetworkSize", "MultiPass", "Temporal",
                                         "Style", "Strength", "MotionVectors", "MgpuHudless" };
    for (const char* k : kUsed)
        if (_stricmp(k, key.c_str()) == 0)
            return true;
    return false;
}

bool Keys::Lookup(const char* key, std::string* value) const
{
    std::string v;
    if (user && user(key, &v) && !IsUnset(v))
    {
        *value = Trim(v);
        return true;
    }
    if (!hasProfile)
        return false;
    std::string pk = key;
    if (multiGpu && _stricmp(key, "Preset") == 0 && profile.keys.count("MgpuPreset"))
        pk = "MgpuPreset";
    const auto it = profile.keys.find(pk);
    if (it == profile.keys.end() || IsUnset(it->second) || !ProfileKeyUsed(pk))
        return false;
    *value = ProfileValue(pk, Trim(it->second));
    fromProfile.insert(key);
    return true;
}

bool Keys::UserSet(const char* key) const
{
    std::string v;
    return user && user(key, &v) && !IsUnset(v);
}

bool Keys::Bool(const char* key, bool def) const
{
    std::string v;
    return Lookup(key, &v) ? ParseBool(v, def) : def;
}

int Keys::Int(const char* key, int def) const
{
    std::string v;
    if (!Lookup(key, &v))
        return def;
    if (Lower(v) == "true")
        return 1;
    if (Lower(v) == "false")
        return 0;
    char* end = nullptr;
    const long n = strtol(v.c_str(), &end, 10);
    return end != v.c_str() ? (int) n : def;
}

float Keys::Float(const char* key, float def) const
{
    std::string v;
    if (!Lookup(key, &v))
        return def;
    char* end = nullptr;
    const float f = strtof(v.c_str(), &end);
    return end != v.c_str() ? f : def;
}

std::wstring Keys::Path(const char* key) const
{
    std::string v;
    if (!user || !user(key, &v) || IsUnset(v))
        return {}; // paths come from the user only
    v = Trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    return Widen(v);
}

std::vector<RuntimeCandidate> BuildCandidates(const DiscoveryInputs& in)
{
    std::vector<RuntimeCandidate> out;
    auto add = [&](const std::wstring& dir, const std::wstring& modules, const std::wstring& weights, std::string src)
    {
        if (dir.empty())
            return;
        for (const auto& c : out)
            if (_wcsicmp(c.dir.c_str(), dir.c_str()) == 0 && _wcsicmp(c.modules.c_str(), modules.c_str()) == 0 &&
                _wcsicmp(c.weights.c_str(), weights.c_str()) == 0)
                return;
        out.push_back({ dir, modules, weights, std::move(src) });
    };
    auto locator = [&](const std::wstring& folder, const std::string& src)
    {
        if (folder.empty())
            return;
        const fs::path base(folder);
        Ini paths;
        if (paths.Load((base / L"paths.ini").wstring()))
        {
            if (const Ini::Section* s = paths.Find("lmxxf-nr"))
            {
                auto get = [&](const char* k) -> std::wstring
                {
                    const auto it = s->find(k);
                    if (it == s->end() || Trim(it->second).empty())
                        return {};
                    fs::path p(Widen(Trim(it->second)));
                    return (p.is_absolute() ? p : base / p).lexically_normal().wstring();
                };
                add(get("RuntimeDir"), get("ModulesDir"), get("WeightsDir"), src + " paths.ini");
            }
        }
        add(folder, L"", L"", src);
    };

    // Ini RuntimeDir with its module / weight folders; ModulesDir alone also applies to the folders searched below
    if (!in.iniRuntime.empty())
        add(in.iniRuntime, in.iniModules, in.iniWeights, "ini RuntimeDir");
    locator(in.envDir, "env LMXXF_NR_DIR");
    locator(in.pluginDir, "plugin folder");
    if (!in.pluginDir.empty())
        locator((fs::path(in.pluginDir) / L"lmxxf_nr").wstring(), "plugin folder");
    locator(in.userDir, "per-user folder");
    add(in.exeDir, in.iniRuntime.empty() ? in.iniModules : L"", in.iniRuntime.empty() ? in.iniWeights : L"",
        "game folder");
    add(in.optiDir, in.iniRuntime.empty() ? in.iniModules : L"", in.iniRuntime.empty() ? in.iniWeights : L"",
        "OptiScaler folder");
    return out;
}

bool VerifySha256Sums(const std::wstring& sumsFile, std::string* error, size_t* files)
{
    *files = 0;
    std::ifstream f(fs::path(sumsFile), std::ios::binary);
    if (!f)
        return *error = "can't read " + Narrow(sumsFile), false;
    const fs::path base = fs::path(sumsFile).parent_path();

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return *error = "BCrypt SHA-256 unavailable", false;
    std::vector<char> buf(1 << 20);
    std::string line;
    bool ok = true;
    while (ok && std::getline(f, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto sp = line.find_first_of(" \t");
        if (sp != 64)
        {
            *error = "SHA256SUMS: malformed line '" + line.substr(0, 80) + "'";
            ok = false;
            break;
        }
        const std::string want = Lower(line.substr(0, 64));
        std::string rel = Trim(line.substr(sp));
        if (!rel.empty() && rel[0] == '*')
            rel.erase(0, 1);
        const fs::path file = base / fs::path(Widen(rel)).make_preferred();
        std::ifstream m(file, std::ios::binary);
        if (!m)
        {
            *error = rel + ": listed in SHA256SUMS but missing";
            ok = false;
            break;
        }
        BCRYPT_HASH_HANDLE h = nullptr;
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
        while (m)
        {
            m.read(buf.data(), (std::streamsize) buf.size());
            if (m.gcount() > 0)
                BCryptHashData(h, (PUCHAR) buf.data(), (ULONG) m.gcount(), 0);
        }
        unsigned char digest[32];
        BCryptFinishHash(h, digest, sizeof(digest), 0);
        BCryptDestroyHash(h);
        char hex[65];
        for (int i = 0; i < 32; ++i)
            snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        if (want != hex)
        {
            *error = rel + ": SHA-256 mismatch";
            ok = false;
            break;
        }
        ++*files;
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (ok && *files == 0)
        return *error = "SHA256SUMS lists no files", false;
    return ok;
}

namespace
{
struct Marker
{
    const wchar_t* name; // file or folder name, exact (case-insensitive)
    const char* what;
};
// Seen in this machine's libraries: EasyAntiCheat\ + start_protected_game.exe (EAC), AntiCheatExpert\ (ACE),
// BlackCipher\ (Nexon), Plugins\EQU8 (EQU8). The rest are the vendors' documented file names.
constexpr Marker kMarkers[] = {
    { L"EasyAntiCheat", "EasyAntiCheat" },
    { L"EasyAntiCheat_EOS", "EasyAntiCheat" },
    { L"EasyAntiCheat_EOS_Setup.exe", "EasyAntiCheat" },
    { L"EasyAntiCheat_Setup.exe", "EasyAntiCheat" },
    { L"start_protected_game.exe", "EasyAntiCheat" },
    { L"BattlEye", "BattlEye" },
    { L"BEService.exe", "BattlEye" },
    { L"BEService_x64.exe", "BattlEye" },
    { L"BEClient.dll", "BattlEye" },
    { L"BEClient_x64.dll", "BattlEye" },
    { L"AntiCheatExpert", "ACE (Anti-Cheat Expert)" },
    { L"BlackCipher", "BlackCipher" },
    { L"EQU8", "EQU8" },
    { L"GameGuard", "nProtect GameGuard" },
    { L"GameGuard.des", "nProtect GameGuard" },
    { L"XIGNCODE", "XIGNCODE3" },
    { L"x3.xem", "XIGNCODE3" },
    { L"mhyprot2.sys", "mhyprot" },
    { L"mhyprot3.sys", "mhyprot" },
    { L"PnkBstrA.exe", "PunkBuster" },
    { L"PnkBstrB.exe", "PunkBuster" },
    { L"FACEIT", "FACEIT" },
    { L"EAAntiCheat.GameServiceLauncher.exe", "EA Javelin" },
    { L"EAAntiCheat.Installer.exe", "EA Javelin" },
    { L"Denuvo Anti-Cheat", "Denuvo Anti-Cheat" },
};

// Titles whose protection leaves no file in the game folder (kernel driver installed elsewhere, or VAC)
struct ExeMarker
{
    const wchar_t* exe;
    const char* what;
};
constexpr ExeMarker kExes[] = {
    { L"VALORANT-Win64-Shipping.exe", "Vanguard" },
    { L"League of Legends.exe", "Vanguard" },
    { L"cs2.exe", "VAC" },
    { L"csgo.exe", "VAC" },
    { L"deadlock.exe", "VAC" },
    { L"dota2.exe", "VAC" },
    { L"FortniteClient-Win64-Shipping.exe", "EasyAntiCheat / BattlEye" },
    { L"r5apex.exe", "EasyAntiCheat" },
    { L"r5apex_dx12.exe", "EasyAntiCheat" },
    { L"RainbowSix.exe", "BattlEye" },
    { L"TslGame.exe", "BattlEye" },
};

constexpr const wchar_t* kModules[] = { L"EasyAntiCheat_x64.dll", L"EasyAntiCheat_EOS.dll", L"BEClient_x64.dll",
                                        L"BEClient.dll", L"x3.x64.xem", L"GameGuard.des" };
} // namespace

std::vector<std::string> DetectAntiCheat(const std::wstring& exeDir, const std::wstring& exeName)
{
    std::vector<std::string> hits;
    for (const auto& e : kExes)
        if (_wcsicmp(e.exe, exeName.c_str()) == 0)
            hits.push_back(std::string(e.what) + " (" + Narrow(exeName) + ")");

    fs::path dir(exeDir);
    for (int up = 0; up <= 3 && !dir.empty(); ++up)
    {
        // Steam library root and drive roots hold other games: stop below them
        const std::wstring leaf = LowerW(dir.filename().wstring());
        if (leaf == L"common" || leaf == L"steamapps" || dir == dir.root_path())
            break;
        for (const fs::path& d : { dir, dir / L"Plugins" })
        {
            for (const auto& m : kMarkers)
            {
                const fs::path p = d / m.name;
                if (Exists(p))
                    hits.push_back(std::string(m.what) + " (" + Narrow(p.wstring()) + ")");
            }
            // BattlEye's launcher: <game>_BE.exe beside the game
            std::error_code ec;
            for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
            {
                const std::wstring n = LowerW(it->path().filename().wstring());
                if (n.size() > 7 && n.compare(n.size() - 7, 7, L"_be.exe") == 0 && IsFile(it->path()))
                    hits.push_back("BattlEye (" + Narrow(it->path().wstring()) + ")");
            }
        }
        dir = dir.parent_path();
    }
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    return hits;
}

std::vector<std::string> LoadedAntiCheatModules()
{
    std::vector<std::string> hits;
    for (const wchar_t* m : kModules)
        if (GetModuleHandleW(m) != nullptr)
            hits.push_back("loaded module " + Narrow(m));
    return hits;
}
} // namespace Autoconfig
