// Profiles, runtime discovery and anti-cheat detection, offline. build.ps1 builds and runs it:
//   main.exe <scratch dir> [--modules <HIP module folder>] [--sweep <game library>...]
#include "../../src/Autoconfig.h"
#include "../../src/Runtime.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace Autoconfig;

static int g_fail = 0;
#define CHECK(c) ((c) ? (void) 0 : (void) (std::printf("FAIL line %d: %s\n", __LINE__, #c), ++g_fail))

static fs::path g_root;

static void Put(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static fs::path Fresh(const char* name)
{
    const fs::path d = g_root / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// sha256 of "abc" and of "hello\n"
static const char* kAbc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
static const char* kHello = "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03";

static void TestIni()
{
    Ini ini;
    ini.Parse("\xEF\xBB\xBF; comment\n[Plugin.lmxxf_nr]\r\nPlacement = 2 ; inline\n# other\nmodel=1\n\n[B]\nk=v=w\n");
    const Ini::Section* s = ini.Find("plugin.LMXXF_NR");
    CHECK(s && s->at("placement") == "2" && s->at("Model") == "1");
    CHECK(ini.Find("B")->at("k") == "v=w");
    CHECK(ini.order.size() == 2 && ini.order[0] == "Plugin.lmxxf_nr");
}

static void TestProfiles(const fs::path& seed)
{
    Ini ini;
    ini.Parse("[Game.exe]\nName=Plain\nPlacement=1\n"
              "[game.EXE|demo]\nName=Demo\nPath=\\Demo\\\n"
              "[Game.exe|demo2]\nName=DemoLonger\nPath=\\Demo\\Bin\n");
    Profile p;
    CHECK(MatchProfile(ini, L"GAME.exe", L"C:\\Games\\X", &p) && p.name == "Plain");
    CHECK(MatchProfile(ini, L"game.exe", L"C:\\Games\\Demo\\Bin\\Win64", &p) && p.name == "DemoLonger");
    CHECK(MatchProfile(ini, L"game.exe", L"C:\\Games\\demo\\Other", &p) && p.name == "Demo");
    CHECK(!MatchProfile(ini, L"other.exe", L"C:\\Games\\X", &p));

    // The seed file: Satisfactory and CONTROL, with no user keys and with user keys
    Ini file;
    CHECK(file.Load(seed.wstring()));
    Keys k;
    std::map<std::string, std::string> user;
    k.user = [&](const char* key, std::string* v)
    {
        auto it = user.find(key);
        return it != user.end() ? (*v = it->second, true) : false;
    };
    CHECK(MatchProfile(file, L"FactoryGameSteam-Win64-Shipping.exe", L"E:\\Satisfactory\\Engine\\Binaries\\Win64",
                       &k.profile));
    k.hasProfile = true;
    CHECK(k.profile.name == "Satisfactory");
    CHECK(k.Int("Placement", 1) == 2 && k.Int("Preset", 0) == 1 && k.Bool("WholeFrameFallback", false));
    CHECK(k.fromProfile.count("Placement") && k.fromProfile.count("Preset"));
    CHECK(k.Int("NetworkSize", 0) == 0 && !k.fromProfile.count("NetworkSize")); // not in the profile: default
    CHECK(k.Int("Quirks", 7) == 7);                   // a profile key this build doesn't read
    CHECK(k.Int("Host.Upscalers.Dx12Upscaler", 7) == 7); // install-time only, never at run time
    k.multiGpu = true;
    CHECK(k.Int("Preset", 0) == 2); // MgpuPreset
    k.multiGpu = false;
    user["Placement"] = "0";
    user["Preset"] = "auto"; // OptiScaler's "unset"
    CHECK(k.Int("Placement", 1) == 0 && k.UserSet("Placement"));
    CHECK(k.Int("Preset", 0) == 1 && !k.UserSet("Preset"));
    user["Preset"] = "3";
    CHECK(k.Int("Preset", 0) == 3);
    CHECK(k.Path("RuntimeDir").empty());
    user["RuntimeDir"] = "\"D:\\nr rt\"";
    CHECK(k.Path("RuntimeDir") == L"D:\\nr rt");

    Keys c;
    CHECK(MatchProfile(file, L"controlresonant.exe", L"E:\\CONTROL Resonant", &c.profile));
    c.hasProfile = true;
    CHECK(c.profile.name == "CONTROL" && c.Int("Placement", 1) == 2 && c.Int("Preset", 0) == 1);
    CHECK(c.Bool("Exposure", false)); // Exposure=game
    c.profile.keys["Exposure"] = "auto";
    CHECK(c.Bool("Exposure", true)); // "auto" is unset everywhere: the built-in default
    c.profile.keys["Exposure"] = "Auto ";
    CHECK(c.Bool("Exposure", true));

    // No profile, no user section: built-in defaults
    Keys none;
    CHECK(none.Int("Placement", 1) == 1 && none.Bool("MultiGpu", false) == false && none.Float("Strength", 1) == 1);
}

static void TestSha()
{
    const fs::path d = Fresh("sha");
    Put(d / "a.bin", "abc");
    Put(d / "sub" / "h.txt", "hello\n");
    std::string err;
    size_t n = 0;
    Put(d / "SHA256SUMS", std::string(kAbc) + "  a.bin\n" + kHello + " *sub/h.txt\n");
    CHECK(VerifySha256Sums((d / "SHA256SUMS").wstring(), &err, &n) && n == 2);
    Put(d / "sub" / "h.txt", "hellO\n");
    CHECK(!VerifySha256Sums((d / "SHA256SUMS").wstring(), &err, &n) && err.find("mismatch") != std::string::npos);
    Put(d / "SHA256SUMS", std::string(kAbc) + "  a.bin\n" + kHello + "  gone.bin\n");
    CHECK(!VerifySha256Sums((d / "SHA256SUMS").wstring(), &err, &n) && err.find("missing") != std::string::npos);
    Put(d / "SHA256SUMS", "nonsense\n");
    CHECK(!VerifySha256Sums((d / "SHA256SUMS").wstring(), &err, &n));
    Put(d / "SHA256SUMS", "\n");
    CHECK(!VerifySha256Sums((d / "SHA256SUMS").wstring(), &err, &n));
}

static void TestDiscovery(const fs::path& realModules)
{
    const fs::path d = Fresh("disc");
    DiscoveryInputs in;
    in.pluginDir = (d / "game" / "OptiScaler" / "plugins").wstring();
    in.exeDir = (d / "game").wstring();
    in.optiDir = (d / "game").wstring();
    in.userDir = (d / "user" / "lmxxf-nr").wstring();
    in.envDir = (d / "env").wstring();

    // Order without paths.ini: env, plugin, plugin\lmxxf_nr, per-user, game (OptiScaler folder = game: deduped)
    auto c = BuildCandidates(in);
    CHECK(c.size() == 5);
    CHECK(c[0].source == "env LMXXF_NR_DIR" && c[1].source == "plugin folder" && c[2].dir == in.pluginDir + L"\\lmxxf_nr");
    CHECK(c[3].source == "per-user folder" && c[4].source == "game folder");

    // ini first; paths.ini (relative to its folder) before its folder
    in.iniRuntime = L"X:\\rt";
    Put(d / "user" / "lmxxf-nr" / "paths.ini", "[lmxxf-nr]\nRuntimeDir=..\\build\\stage\nModulesDir=D:\\m\nWeightsDir=w\n");
    c = BuildCandidates(in);
    CHECK(c[0].source == "ini RuntimeDir" && c[0].dir == L"X:\\rt");
    size_t pi = 0;
    for (size_t i = 0; i < c.size(); ++i)
        if (c[i].source == "per-user folder paths.ini")
            pi = i;
    CHECK(pi > 0 && c[pi].dir == (d / "user" / "build" / "stage").lexically_normal().wstring());
    CHECK(c[pi].modules == L"D:\\m" && c[pi].weights == (d / "user" / "lmxxf-nr" / "w").wstring());
    CHECK(c[pi + 1].source == "per-user folder");
    in.iniRuntime.clear();

    // Layouts. A candidate with a bad module hash is skipped for the next one.
    const std::string sums = std::string(kAbc) + "  gfx1201/m.hsaco\n";
    RuntimeLayout r;
    // 1. Nothing anywhere
    CHECK(!ResolveRuntime({ in.pluginDir }, in.exeDir, &r) && r.error == "no LmxxfNrRuntime.dll");
    // 2. dll without assets
    Put(d / "env" / "LmxxfNrRuntime.dll", "x");
    CHECK(!ResolveRuntime({ in.envDir }, in.exeDir, &r) && r.error.find("no assets") != std::string::npos);
    // 3. dll + modules\ with a bad hash
    Put(d / "env" / "modules" / "SHA256SUMS", sums);
    Put(d / "env" / "modules" / "gfx1201" / "m.hsaco", "abd");
    CHECK(!ResolveRuntime({ in.envDir }, in.exeDir, &r) && r.error.find("mismatch") != std::string::npos);
    // 4. fixed
    Put(d / "env" / "modules" / "gfx1201" / "m.hsaco", "abc");
    CHECK(ResolveRuntime({ in.envDir }, in.exeDir, &r) && r.layout == (int) Runtime::Layout::LmxxfBuild && r.hashed == 1);
    // 5. lmxxf's package in the per-user folder: DLSS5-AMD\LmxxfNrRuntime.dll + native-game-tiled-assets\HIP
    const fs::path pkg = d / "user" / "lmxxf-nr" / "DLSS5-AMD";
    Put(pkg / "LmxxfNrRuntime.dll", "x");
    Put(pkg / "native-game-tiled-assets" / "HIP" / "SHA256SUMS", sums);
    Put(pkg / "native-game-tiled-assets" / "HIP" / "gfx1201" / "m.hsaco", "abc");
    CHECK(ResolveRuntime({ in.userDir }, in.exeDir, &r) && r.layout == (int) Runtime::Layout::LmxxfPackage);
    CHECK(r.dll == (pkg / "LmxxfNrRuntime.dll").wstring());
    // 6. runtime + pak beside the plugin (no SHA256SUMS to check)
    Put(fs::path(in.pluginDir) / "LmxxfNrRuntime.dll", "x");
    Put(fs::path(in.pluginDir) / "LmxxfNrRuntime.pak", "x");
    CHECK(ResolveRuntime({ in.pluginDir }, in.exeDir, &r) && r.layout == (int) Runtime::Layout::RuntimePak && r.hashed == 0);
    // 7. explicit modules folder wins over the pak
    CHECK(ResolveRuntime({ in.pluginDir, (d / "env" / "modules").wstring() }, in.exeDir, &r) &&
          r.layout == (int) Runtime::Layout::LmxxfBuild);
    // 8. Runtime::Load over the list reports each rejected candidate, in order
    fs::remove(fs::path(in.pluginDir) / "LmxxfNrRuntime.pak");
    Put(d / "env" / "modules" / "gfx1201" / "m.hsaco", "abd");
    RuntimePaths paths { BuildCandidates(in), in.exeDir };
    std::vector<std::string> why;
    for (const auto& cand : paths.candidates)
        if (!ResolveRuntime(cand, paths.exe, &r))
            why.push_back(cand.source + ": " + r.error);
        else
        {
            CHECK(cand.source == "per-user folder");
            break;
        }
    CHECK(why.size() == 4 && why[0].find("mismatch") != std::string::npos && why[1].find("no assets") != std::string::npos);

    // The real module folder of the lmxxf build, when given: every module hashes right
    if (!realModules.empty())
    {
        std::string err;
        size_t n = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = VerifySha256Sums((realModules / "SHA256SUMS").wstring(), &err, &n);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("real modules: %s, %zu files, %.0f ms %s\n", ok ? "ok" : "FAILED", n, ms, err.c_str());
        CHECK(ok && n > 0);
    }
}

static void TestAntiCheat()
{
    const fs::path d = Fresh("ac") / "steamapps" / "common";
    // EAC beside the exe (FromSoftware layout)
    Put(d / "Ac6" / "Game" / "EasyAntiCheat" / "easyanticheat_eos_setup.exe", "x");
    Put(d / "Ac6" / "Game" / "start_protected_game.exe", "x");
    auto h = DetectAntiCheat((d / "Ac6" / "Game").wstring(), L"armoredcore6.exe");
    CHECK(h.size() == 2 && h[0].rfind("EasyAntiCheat", 0) == 0);
    // EAC at the root of an Unreal game, exe three levels down
    Put(d / "Hunt" / "EasyAntiCheat" / "EasyAntiCheat_EOS_Setup.exe", "x");
    Put(d / "Hunt" / "bin" / "win64" / "hunt.exe", "x");
    CHECK(DetectAntiCheat((d / "Hunt" / "bin" / "win64").wstring(), L"hunt.exe").size() == 1);
    // Plugins\EQU8, ACE, BlackCipher, BattlEye launcher
    fs::create_directories(d / "Sg" / "PortalWars" / "Plugins" / "EQU8");
    CHECK(DetectAntiCheat((d / "Sg" / "PortalWars" / "Binaries" / "Win64").wstring(), L"x.exe").size() == 1);
    fs::create_directories(d / "Df" / "Game" / "DeltaForce" / "Binaries" / "Win64" / "AntiCheatExpert");
    CHECK(DetectAntiCheat((d / "Df" / "Game" / "DeltaForce" / "Binaries" / "Win64").wstring(), L"x.exe").size() == 1);
    Put(d / "Be" / "Game_BE.exe", "x");
    h = DetectAntiCheat((d / "Be").wstring(), L"Game.exe");
    CHECK(h.size() == 1 && h[0].rfind("BattlEye", 0) == 0);
    // Known exe names
    CHECK(DetectAntiCheat((d / "Nothing").wstring(), L"VALORANT-Win64-Shipping.exe").size() == 1);
    // Clean: the EOS SDK is not anti-cheat; a neighbour game's EAC is not looked at
    Put(d / "Sat" / "Engine" / "Binaries" / "Win64" / "EOSSDK-Win64-Shipping.dll", "x");
    CHECK(DetectAntiCheat((d / "Sat" / "Engine" / "Binaries" / "Win64").wstring(), L"FactoryGameSteam-Win64-Shipping.exe").empty());
    Put(d / "EasyAntiCheat" / "x", "x"); // in the library root: never reached
    CHECK(DetectAntiCheat((d / "Clean").wstring(), L"clean.exe").empty());
    CHECK(LoadedAntiCheatModules().empty());
}

// Every exe under each library's games, read-only: which games the plugin would refuse
static void Sweep(const fs::path& lib)
{
    std::error_code ec;
    for (fs::directory_iterator g(lib, ec), end; !ec && g != end; g.increment(ec))
    {
        if (!g->is_directory())
            continue;
        std::set<std::string> hits;
        size_t exes = 0;
        fs::recursive_directory_iterator it(g->path(), fs::directory_options::skip_permission_denied, ec), e2;
        for (; !ec && it != e2; it.increment(ec))
        {
            if (it.depth() > 4)
            {
                it.disable_recursion_pending();
                continue;
            }
            std::wstring ext = it->path().extension().wstring();
            if (_wcsicmp(ext.c_str(), L".exe") != 0)
                continue;
            ++exes;
            for (const auto& s : DetectAntiCheat(it->path().parent_path().wstring(), it->path().filename().wstring()))
                hits.insert(s.substr(0, s.find(" (")));
        }
        std::string list;
        for (const auto& s : hits)
            list += (list.empty() ? "" : ", ") + s;
        std::printf("  %-45s %3zu exe  %s\n", Narrow(g->path().filename().wstring()).c_str(), exes,
                    hits.empty() ? "-" : list.c_str());
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return std::printf("usage: main.exe <scratch dir> [--modules dir] [--sweep lib...]\n"), 2;
    g_root = fs::absolute(argv[1]).lexically_normal();
    fs::path modules;
    std::vector<fs::path> sweeps;
    for (int i = 2; i + 1 < argc; ++i)
    {
        if (!strcmp(argv[i], "--modules"))
            modules = argv[++i];
        else if (!strcmp(argv[i], "--sweep"))
            sweeps.emplace_back(argv[++i]);
    }
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const fs::path seed = fs::path(exe).parent_path() / ".." / ".." / "profiles" / "lmxxf_nr_profiles.ini";

    TestIni();
    TestProfiles(seed);
    TestSha();
    TestDiscovery(modules);
    TestAntiCheat();
    for (const auto& s : sweeps)
    {
        std::printf("sweep %s\n", s.string().c_str());
        Sweep(s);
    }
    std::error_code ec;
    fs::remove_all(g_root / "sha", ec), fs::remove_all(g_root / "disc", ec), fs::remove_all(g_root / "ac", ec);
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
