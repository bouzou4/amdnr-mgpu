#pragma once
// AMDNR gap (docs/AMDNR_GAPS.md 3.4): the 0.3.5 NR looks and Neural placement, rebuilt as parameter presets from
// AMDNR's published 0.3.5.8 docs (README "NR style" / "Neural placement", OptiScaler.ini AmdNrLook / AmdPlacement).
// A look fills only the keys the user left on auto, as volatile values, so Save Settings never writes them.
#include <Config.h>

#include <algorithm>
#include <cmath>

namespace DlssNr::Looks
{
enum Look : int
{
    FullModel = -1,
    Nvidia = 0,
    Soft = 1,
};

inline int Current()
{
    const int v = Config::Instance()->AmdNrLook.value_or_default();
    return v == Nvidia || v == Soft ? v : FullModel;
}

inline const char* Name(int look)
{
    return look == Nvidia ? "NVIDIA look" : look == Soft ? "AMDNR soft" : "Full Model Look";
}

// Post (after upscaling) or pre: AmdPlacement when set, otherwise the look's own (Full Model Look asks for post).
inline bool PostPlacement()
{
    const int p = Config::Instance()->AmdPlacement.value_or_default();
    if (p == 0 || p == 1)
        return p == 1;
    return Current() == FullModel;
}

namespace detail
{
template <class T> struct Slot
{
    CustomOptional<T> Config::*member;
    bool supplied = false;
    T value {};
};
inline Slot<float> floats[] = { { &Config::DlssNrLocalStructure }, { &Config::AmdLmxxfEditDetail },
                                { &Config::AmdLmxxfEditSaturation }, { &Config::AmdResidualLimit },
                                { &Config::AmdLmxxfEdgeGuard },       { &Config::AmdTemporalStability },
                                { &Config::AmdLmxxfOutputSmooth } };
inline Slot<int> style { &Config::AmdRuntimeStyle };
inline Slot<bool> fullNetwork { &Config::LmxxfFullNetwork };

// A key is the user's when it holds a value the look did not put there.
template <class T> void Fill(Config& c, Slot<T>& s, T v)
{
    auto& key = c.*s.member;
    const bool ours = s.supplied && key.has_value() && key.value() == s.value;
    if (key.has_value() && !ours)
    {
        s.supplied = false;
        return;
    }
    key.set_volatile_value(v);
    s.supplied = true;
    s.value = v;
}
} // namespace detail

// Values per look (README 0.3.5.8): structure, edit detail, edit colour, residual limit, edge guard,
// temporal stability, output smoothing; then the lmxxf network style and Full network.
inline void Apply()
{
    using namespace detail;
    auto& c = *Config::Instance();
    static constexpr float kValues[3][7] = {
        { 2.0f, 1.5f, 1.25f, 0.0f, 0.0f, 0.5f, 0.6f },  // Full Model Look
        { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.6f, 0.6f },   // NVIDIA look
        { 1.0f, 1.0f, 1.0f, 0.25f, 0.5f, 0.6f, 0.6f },  // AMDNR soft (0.3.4 values)
    };
    const int look = Current();
    const int row = look == FullModel ? 0 : look == Nvidia ? 1 : 2;
    for (int i = 0; i < 7; ++i)
        Fill(c, floats[i], kValues[row][i]);
    Fill(c, style, look == Soft ? 1 : 0);
    Fill(c, fullNetwork, look != Soft);
}

// True while every value the look fills is still the look's.
inline bool Untouched()
{
    for (const auto& f : detail::floats)
        if (!f.supplied)
            return false;
    return detail::style.supplied;
}

// Menu pick: the look's values replace the sliders; its placement and Full network become the user's own values
// (README: [LOOKPICK]).
inline void Pick(int look)
{
    auto& c = *Config::Instance();
    for (auto& f : detail::floats)
        (c.*f.member) = std::optional<float> {};
    c.AmdRuntimeStyle = std::optional<int> {};
    c.AmdNrLook = look;
    c.AmdPlacement = look == FullModel ? 1 : 0;
    c.LmxxfFullNetwork = look != Soft;
    detail::fullNetwork.supplied = false;
    Apply();
}

// Face detail: while Character structure follows Structure intensity, faces get 1.5x it, at most 4.
inline float SkinStructure(float structure, float skinKey)
{
    if (skinKey >= 0.f)
        return skinKey;
    if (!Config::Instance()->AmdFaceDetail.value_or_default() || !std::isfinite(structure))
        return structure;
    return std::min(structure * 1.5f, 4.0f);
}
} // namespace DlssNr::Looks
