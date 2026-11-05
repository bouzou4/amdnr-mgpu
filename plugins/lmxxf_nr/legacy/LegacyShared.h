// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
// Legacy mode: the block a 32-bit game's d3d9 proxy and the x64 NR host share. Same layout in both builds.
#pragma once
#include <cstdint>

namespace Legacy
{
constexpr uint32_t kMagic = 0x4C4E5231; // "LNR1"
constexpr uint32_t kVersion = 1;

// Kernel object names, %u = game process id (and texture generation)
constexpr const wchar_t kBlockName[] = L"Local\\lmxxf_legacy_%u";
constexpr const wchar_t kFenceName[] = L"Local\\lmxxf_legacy_%u_fence";
constexpr const wchar_t kFrameName[] = L"Local\\lmxxf_legacy_%u_frame_%u";

// Fence protocol: frame n, the proxy signals 2n-1 once its copy of the back buffer is queued, the host 2n once the
// result is in the same texture. The proxy waits on the CPU for 2n (with a timeout) before copying it back.
#pragma pack(push, 8)
struct Block
{
    uint32_t magic, version;
    // Proxy
    uint32_t gamePid;
    uint32_t adapterLuidLow;
    int32_t adapterLuidHigh;
    uint32_t generation;          // frame texture: bumped whenever it is recreated
    uint32_t width, height, format; // DXGI_FORMAT of the frame texture
    uint32_t colorSpace;          // 1 sRGB-encoded
    uint64_t frame;               // n of the last frame signalled (2n-1)
    uint32_t reset;               // history break requested (resize, device reset)
    uint32_t pad0;
    // Host
    uint32_t hostPid;
    uint32_t hostReady;           // fence opened, NR loaded or failed (answers still come)
    uint32_t hostGeneration;      // generation the host has open
    int32_t hostState;            // 0 starting, 1 NR running, 2 pass-through (NR failed or off)
    float hostMs;                 // host GPU time per frame (OF + NR), median
    float ofMs;                   // optical flow GPU time, median
    float flkStatic, flkMc;       // flicker meter, NAN when none
    char status[192];
};
#pragma pack(pop)
} // namespace Legacy
