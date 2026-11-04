// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
// Performance presets: each one only sets the Model, Network size, Passes and history rows.
#pragma once

#include "Nr.h"

enum class Preset : int32_t
{
    Custom = 0,
    Performance = 1,
    Balanced = 2,
    Quality = 3,
    Ultra = 4,
};

struct PresetRows
{
    Preset id;
    const char* name;
    int32_t model; // 0 Full, 1 Small
    NetworkSize size;
    int32_t passes;
    bool temporal;
    const char* cost; // single-GPU network time, 4K output
};

// Ordered by cost and quality; network times from the README's terminal runs at 4K output
inline constexpr PresetRows kPresets[] = {
    { Preset::Performance, "Performance", 1, NetworkSize::Tier1080, 1, true,
      "about 8 ms of network (measured: 55 fps, 76 without NR)" },
    { Preset::Balanced, "Balanced", 0, NetworkSize::Tier1080, 1, true,
      "about 18 ms of network (measured: 48 fps)" },
    { Preset::Quality, "Quality", 0, NetworkSize::Native1440, 1, true,
      "about 25 ms of network (measured: 35 fps)" },
    { Preset::Ultra, "Ultra", 0, NetworkSize::Native, 1, true,
      "about 55 ms of network (estimated, 2.25x the pixels of Quality at 4K)" },
};
inline constexpr int32_t kPresetCount = int32_t(sizeof(kPresets) / sizeof(kPresets[0]));

struct PresetCaps
{
    bool freeRes = false; // DLSS5_NETWORK_FREE_RES: 1440 and native sizes
    bool models = false;  // Full / Small choice
};

inline const PresetRows* FindPreset(Preset p)
{
    for (const auto& r : kPresets)
        if (r.id == p)
            return &r;
    return nullptr;
}

inline void ApplyPreset(NrSettings& s, Preset p)
{
    if (const PresetRows* r = FindPreset(p))
    {
        s.model = r->model;
        s.size = r->size;
        s.passes = r->passes;
        s.temporal = r->temporal;
    }
}

// The preset whose rows the settings hold, Custom when none
inline Preset MatchPreset(const NrSettings& s)
{
    for (const auto& r : kPresets)
        if (s.model == r.model && s.size == r.size && s.passes == r.passes && s.temporal == r.temporal)
            return r.id;
    return Preset::Custom;
}

// Why the runtime can't run a preset, nullptr when it can
inline const char* PresetBlocked(Preset p, const PresetCaps& c)
{
    const PresetRows* r = FindPreset(p);
    if (r == nullptr)
        return nullptr;
    if (r->size != NetworkSize::Tier1080 && !c.freeRes)
        return "needs DLSS5_NETWORK_FREE_RES (1440 / native sizes), not in this runtime";
    if (r->model == 1 && !c.models)
        return "needs the Small model (DLSS5_MULTI_PASS), not in this runtime";
    return nullptr;
}

inline Preset PresetFromIni(int32_t v)
{
    return v >= 1 && v <= kPresetCount ? Preset(v) : Preset::Custom;
}
