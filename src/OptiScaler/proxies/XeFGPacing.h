#pragma once
// AMDNR gap (docs/AMDNR_GAPS.md): AMDNR's XeFG pacing engine and MFG unlock are not in its published
// source. These stubs keep its call sites compiling with both off: XeFG runs as upstream (6X at most).
#include <windows.h>
#include <cstdint>

namespace XeFGPacing
{
inline void SetSwapchainWaitable(HANDLE) {}
inline void OnContextDestroyed() {}
inline void OnActivate(int) {}
inline void OnDeactivate() {}
inline void OnMultiplierChange(int) {}
inline double RenderTimeMs() { return 0.0; }
inline void NoteFedFrameTime(double) {}
inline bool MfgActive() { return false; }
inline bool HoldInterleavePacing() { return false; }
inline void NoteOverlayTime(int64_t) {}
} // namespace XeFGPacing

namespace XeFGUnlock
{
inline constexpr int UnpacedMaxInterpolations = 5;
inline void ApplyToModule(HMODULE, bool) {}
inline void ApplyToAllLoaded() {}
inline int OursCeiling() { return 0; }
inline bool OursCappedAsGameCopy() { return false; }
inline bool OursCappedWithoutPacing() { return false; }
inline bool Applied() { return false; }
inline int UnlockedModules() { return 0; }
inline bool Attempted() { return false; }
} // namespace XeFGUnlock
