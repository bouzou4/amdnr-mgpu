// Modifications Copyright (c) 2026 3zwr1 (AMDNR)
#pragma once

#include "SysUtils.h"

#include <flag-set-cpp/flag_set.hpp>

// Game specific hacks that are checked deeper in code
// Per-game config defaults and the exe -> quirk table live in GameConfigs.cpp
enum class GameQuirk : uint64_t
{
    CyberpunkHudlessState,
    FSRFGHudlessMismatchFixup, // Shader extracts UI from swapchain, alpha cut off and apply to hudless
    SkipFsr3Method,
    FastFeatureReset,
    LoadD3D12Manually,
    LoadVulkanManually,
    KernelBaseHooks,
    VulkanDLSSBarrierFixup,
    ForceUnrealEngine,
    NoFSRFGFirstSwapchain,
    FixSlSimulationMarkers,
    HitmanReflexHacks,
    SkipD3D11FeatureLevelElevation,
    CreateD3D12DeviceForLuma,
    ForceCreateD3D12Device,
    ForceDepthD32S8,
    PregmataFixDLSSModes,
    IgnoreValidUntilEvaluateForFG,
    IgnoreTagsWithoutHudlessForFG,
    ForceFGRenderSizeMVs,
    CreateSLOnThe2ndDevice,
    XeFGCameraMotionFill,      // XeFG inputs: velocity has alpha 0 where only the camera moved (The Witcher 3)
    DLSSGFrameLatencyWaitable, // Game waits on the frame latency waitable, SL must not consume it too
    HideFsr4,       // FFX inputs: game's options menu breaks when every upscaler it knows is available (BeamNG)
    ExtendedSearch, // Extend the search when normal lookup fails (Phantom Blade Zero)
    // Marked a path-traced title. It labelled the FSR-RR path-traced profile's checkbox ("opt-in for this game") and
    // named it in Active Quirks; the profile is opt-in since 0.3.3.1 ([FSR-RR] FfxDenoiserPathTracedProfile) and
    // retired in 0.3.4 (RR-25: worse in every RE Requiem report), so no entry sets the bit any more (it invited RE
    // Requiem players to tick a retired profile). The member stays so no later value moves; a later profile (RR-18)
    // can set it again
    PathTracedRayReconstruction,
    // Config-level: FSR-RR skin smoothing ([FSR-RR] FfxDenoiserSkinSmoothing) on by default
    SkinSmoothingDefault,
    // Config-level: FSR-RR bias-mask routing ([FSR-RR] FfxDenoiserBiasMaskStrength) 0 by default
    RrBiasMaskDefaultOff,
    // Warn-only: Capcom's anti-tamper needs REFramework next to OptiScaler (misc/REFrameworkCheck.h)
    NeedsREFramework,
    // Don't forget to add the new entry to printQuirks
    _
};
