// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
// Legacy mode: the block a 32-bit game's d3d9 proxy and the x64 NR host share. Same layout in both builds.
#pragma once
#include <cstddef>
#include <cstdint>

namespace Legacy
{
constexpr uint32_t kMagic = 0x4C4E5231; // "LNR1"
constexpr uint32_t kVersion = 2;

// Kernel object names, %u = game process id (and texture generation)
constexpr const wchar_t kBlockName[] = L"Local\\lmxxf_legacy_%u";
constexpr const wchar_t kFenceName[] = L"Local\\lmxxf_legacy_%u_fence";
constexpr const wchar_t kFrameName[] = L"Local\\lmxxf_legacy_%u_frame_%u";

// Host settings the proxy's menu and hotkeys write; the host applies them when serial changes. Same meaning and
// ranges as the [Host] ini keys.
struct Control
{
    uint32_t serial;
    int32_t model;       // 0 Full, 1 Small
    int32_t size;        // NetworkSize: 0 1080 tier, 1 1440, 2 native
    int32_t passes;      // 1..3 (Full)
    int32_t style;       // -1 auto, 0..2
    int32_t temporal;    // network history
    int32_t flow;        // 0 none, 1 zero vectors, 2 optical flow
    int32_t upsample;    // EditUpsample: 0 bilinear, 1 guided
    int32_t stabilise;   // EditStabilise
    float strength;      // 0..2
    float colorStrength; // -1 auto, 0..1
    uint32_t pad;
};

// Capability bits the host reports
constexpr uint32_t kCapFreeRes = 1; // 1440 and native sizes
constexpr uint32_t kCapModels = 2;  // Full / Small and passes
constexpr uint32_t kCapStyles = 4;

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
    // v2: live settings (proxy writes) and readouts (host writes, every 500 ms)
    Control ctl;
    uint32_t hostSerial;          // ctl.serial the host applied
    uint32_t caps;                // kCap*
    uint32_t feedWidth, feedHeight; // network resolution
    float netMs;                  // network only (runtime timings), median
    float waitMs;                 // host CPU wait for the network answer, last frame
    uint64_t nrFrames;            // frames the host ran NR on
    uint64_t passFrames;          // frames the host answered without NR (rebuild, failure)
    char error[128];
};
#pragma pack(pop)
static_assert(offsetof(Block, frame) == 40 && offsetof(Block, ctl) == 280 && offsetof(Block, nrFrames) == 352,
              "layout differs between builds");
static_assert(sizeof(Control) == 48 && sizeof(Block) == 496, "layout differs between builds");
} // namespace Legacy
