// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
// Drop-in configuration: per-game profiles, runtime discovery, anti-cheat refusal. No OptiScaler or D3D
// dependency, so tools/autoconfig_test builds it alone.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Autoconfig
{
// Minimal ini: sections and keys case-insensitive, ';' / '#' comments, last value wins
struct Ini
{
    struct Less
    {
        bool operator()(const std::string& a, const std::string& b) const;
    };
    using Section = std::map<std::string, std::string, Less>;
    std::map<std::string, Section, Less> sections;
    std::vector<std::string> order; // section names in file order

    bool Load(const std::wstring& path);
    void Parse(const std::string& text);
    const Section* Find(const std::string& section) const;
};

struct Profile
{
    std::string section; // the exe name it matched
    std::string name;
    Ini::Section keys;
};

// Profile for exeName (case-insensitive) in exeDir. A section with Path= matches only when exeDir contains it;
// the longest Path wins, a section without Path is the fallback.
bool MatchProfile(const Ini& ini, const std::wstring& exeName, const std::wstring& exeDir, Profile* out);

// Keys the profile may set that this plugin build reads
bool ProfileKeyUsed(const std::string& key);

// Setting lookup: [Plugin.lmxxf_nr] (the user) > profile > built-in default. "auto" or empty counts as unset.
struct Keys
{
    std::function<bool(const char* key, std::string* value)> user;
    Profile profile;
    bool hasProfile = false;
    bool multiGpu = false;                   // Preset reads the profile's MgpuPreset
    mutable std::set<std::string> fromProfile; // keys a profile value was returned for

    bool Lookup(const char* key, std::string* value) const;
    bool UserSet(const char* key) const;
    bool Bool(const char* key, bool def) const;
    int Int(const char* key, int def) const;
    float Float(const char* key, float def) const;
    std::wstring Path(const char* key) const;
};

// Runtime search
struct RuntimeCandidate
{
    std::wstring dir;     // where LmxxfNrRuntime.dll is looked for (and dir\DLSS5-AMD)
    std::wstring modules; // explicit HIP module folder (SHA256SUMS), optional
    std::wstring weights; // explicit tiled weights folder, optional
    std::string source;   // "ini", "env LMXXF_NR_DIR", "plugin folder", ...
};

struct DiscoveryInputs
{
    std::wstring iniRuntime, iniModules, iniWeights; // [Plugin.lmxxf_nr] RuntimeDir / ModulesDir / WeightsDir
    std::wstring envDir;                             // LMXXF_NR_DIR
    std::wstring pluginDir;                          // folder of lmxxf_nr.asi
    std::wstring userDir;                            // %LOCALAPPDATA%\lmxxf-nr
    std::wstring exeDir, optiDir;
};

// In search order: ini, env, plugin folder, per-user folder, game and OptiScaler folders. A locator folder (env,
// plugin, per-user) may hold paths.ini ([lmxxf-nr] RuntimeDir / ModulesDir / WeightsDir, relative to it), tried first.
std::vector<RuntimeCandidate> BuildCandidates(const DiscoveryInputs& in);

// Every file SHA256SUMS lists, hashed. false with the first bad or missing file in *error.
bool VerifySha256Sums(const std::wstring& sumsFile, std::string* error, size_t* files);

// Anti-cheat in the game: marker files and folders in exeDir and up to 3 parents (and their Plugins\), known
// exe names, and modules already loaded. Each hit as "EasyAntiCheat (path)".
std::vector<std::string> DetectAntiCheat(const std::wstring& exeDir, const std::wstring& exeName);
std::vector<std::string> LoadedAntiCheatModules();

std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);
} // namespace Autoconfig

extern Autoconfig::Keys g_keys;
