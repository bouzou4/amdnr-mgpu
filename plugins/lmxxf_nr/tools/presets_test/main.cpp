// Preset -> rows mapping and Custom detection. From a VS x64 prompt in this folder:
//   cl /std:c++20 /EHsc /nologo /I..\..\src main.cpp && main.exe
#include "Presets.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(c) ((c) ? (void) 0 : (void) (std::printf("FAIL line %d: %s\n", __LINE__, #c), ++g_fail))

int main()
{
    // Each preset round-trips and sets exactly its rows
    for (const auto& r : kPresets)
    {
        NrSettings s;
        s.strength = 0.5f, s.style = 2, s.placement = Placement::Terminal;
        ApplyPreset(s, r.id);
        CHECK(MatchPreset(s) == r.id);
        CHECK(s.model == r.model && s.size == r.size && s.passes == r.passes && s.temporal == r.temporal);
        CHECK(s.strength == 0.5f && s.style == 2 && s.placement == Placement::Terminal);
        CHECK(PresetFromIni((int32_t) r.id) == r.id);
    }
    // Rows are distinct, and cost does not fall along the list
    for (int i = 1; i < kPresetCount; ++i)
    {
        const auto &a = kPresets[i - 1], &b = kPresets[i];
        CHECK(a.model != b.model || a.size != b.size || a.passes != b.passes);
        CHECK(b.model <= a.model && (int) b.size >= (int) a.size && b.passes >= a.passes);
    }
    // Any row edit after a preset reads Custom; look rows don't
    {
        NrSettings s;
        ApplyPreset(s, Preset::Balanced);
        s.strength = 1.5f, s.style = 1;
        CHECK(MatchPreset(s) == Preset::Balanced);
        NrSettings t = s;
        t.temporal = false;
        CHECK(MatchPreset(t) == Preset::Custom);
        t = s, t.passes = 2;
        CHECK(MatchPreset(t) == Preset::Custom);
        t = s, t.size = NetworkSize::Native;
        CHECK(MatchPreset(t) == Preset::Ultra); // the rows are Ultra's
        t.model = 1;
        CHECK(MatchPreset(t) == Preset::Custom);
        t = s, t.model = 1;
        CHECK(MatchPreset(t) == Preset::Performance); // the rows are Performance's
        ApplyPreset(t, Preset::Quality);
        CHECK(MatchPreset(t) == Preset::Quality); // picking again overwrites
    }
    CHECK(PresetFromIni(0) == Preset::Custom && PresetFromIni(9) == Preset::Custom && PresetFromIni(-1) == Preset::Custom);
    // Capabilities
    const PresetCaps none {}, all { true, true }, noFree { false, true }, noModels { true, false };
    for (const auto& r : kPresets)
        CHECK(PresetBlocked(r.id, all) == nullptr);
    CHECK(PresetBlocked(Preset::Performance, noFree) == nullptr && PresetBlocked(Preset::Balanced, noFree) == nullptr);
    CHECK(PresetBlocked(Preset::Quality, noFree) != nullptr && PresetBlocked(Preset::Ultra, noFree) != nullptr);
    CHECK(PresetBlocked(Preset::Performance, noModels) != nullptr && PresetBlocked(Preset::Balanced, noModels) == nullptr);
    CHECK(PresetBlocked(Preset::Balanced, none) == nullptr && PresetBlocked(Preset::Custom, none) == nullptr);
    std::printf(g_fail ? "%d failed\n" : "presets: all checks passed\n", g_fail);
    return g_fail != 0;
}
