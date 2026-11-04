#pragma once
// AMDNR gap stub (docs/AMDNR_GAPS.md): FSR Ray Regeneration's status block, read by the Neural menu.
// The RR feature that writes it is not in AMDNR's published source, so every value stays "no RR frame".
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FSRD::RuntimeStatus
{
struct DebugView
{
    const char* Name = nullptr;
    uint64_t Value = 0;
};

struct DebugViewTable
{
    const DebugView* Views = nullptr;
    size_t Count = 0;
};

inline std::atomic<int> BiasMaskPresent { -1 };
inline std::atomic<int> SssGuide { -1 };
inline std::atomic<bool> SkinSmoothingUnavailable { false };
inline std::atomic<int> SssGuideRejection { 0 };
inline std::atomic<const DebugViewTable*> DebugViews { nullptr };
inline std::atomic<bool> RayRegenOffForSession { false };
inline std::atomic<uint32_t> ResizeRecreateHandle { 0 };
} // namespace FSRD::RuntimeStatus
