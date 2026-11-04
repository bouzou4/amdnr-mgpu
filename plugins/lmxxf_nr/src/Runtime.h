// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstddef>
#include <string>

#include "../third_party/lmxxf/LmxxfNrApi.h"

// The user-installed LmxxfNrRuntime.dll, through lmxxf's C ABI v1
struct Runtime
{
    enum class Layout
    {
        None,
        LmxxfPackage, // DLSS5-AMD\native-game-tiled-assets (lmxxf's own package)
        RuntimePak,   // LmxxfNrRuntime.pak beside the dll (a repackaged runtime, compatibility path)
    };

    HMODULE module = nullptr;
    LmxxfNrApi api {};
    bool hasTimings = false; // the table has GetTimings (lmxxf's current header)
    size_t exports = 0;
    uint32_t tableSize = 0;   // the function table size the runtime accepted
    uint64_t table[64] = {}; // that table when it is larger than lmxxf's
    Layout layout = Layout::None;
    std::wstring dllPath;
    std::wstring assets; // what Create gets as assets_directory
    LmxxfNrCapabilities caps {};
    std::string error;

    // Finds and loads the runtime. dirs: semicolon separated folders tried first (the RuntimeDir setting)
    bool Load(const std::wstring& extraDir, const std::wstring& exeDir, const std::wstring& optiDir);

    // The frame info sizes lmxxf's header defines, newest first
    static constexpr uint32_t kFrameInfoSizes[] = { sizeof(LmxxfNrFrameInfo), LMXXF_NR_FRAME_INFO_V1_SIZE, 64u };

    std::string LastError() const;
    const char* LayoutName() const;
    // DLSS5_* settings the binary knows (found by name in the dll): free network size, style, model
    bool envFreeRes = false;
    bool envStyle = false;
    bool envMultiPass = false;
    int temporal = -1; // motion-fed history (LMXXF_NR_FRAME_FLAG_TEMPORAL): -1 not tried, 0 refused, 1 taken
    uint32_t temporalSize = 0;
};

// Motion-fed network history: not in lmxxf's published header. The layout of the lmxxf temporal build and of
// runtimes that share its offsets (the first 80 bytes are LmxxfNrFrameInfo); used only when PrepareFrame takes it.
#define LMXXF_NR_FRAME_FLAG_TEMPORAL_EXT (1u << 3)
struct LmxxfNrFrameInfoTemporalExt
{
    uint32_t struct_size;
    uint64_t session_id;
    uint64_t frame_id;
    uint64_t list_generation;
    void* command_list;
    uint32_t color_width;
    uint32_t color_height;
    void* color;
    uint32_t color_state;
    uint32_t flags;
    float transfer_strength;
    float color_strength;
    uint32_t debug_view;
    float model_scale;
    void* motion;            // 80: two float channels, motion * scale = colour pixels to the previous frame
    uint32_t motion_state;   // 88
    uint32_t motion_width;   // 92: texels used, 0 = the resource's size
    uint32_t motion_height;  // 96
    float motion_scale_x;    // 100
    float motion_scale_y;    // 104
    uint32_t history_reset;  // 108
    uint32_t passes;         // 112
    float output_smooth;     // 116
    float output_smooth_threshold; // 120
    float reserved_controls[5];    // 124
    void* exposure;          // 144
    uint32_t exposure_state; // 152
    float pre_exposure;      // 156
    float exposure_scale;    // 160
};
static_assert(offsetof(LmxxfNrFrameInfoTemporalExt, motion) == 80 && sizeof(LmxxfNrFrameInfoTemporalExt) == 168);

// Sets a DLSS5_* variable for the runtime: the Win32 environment and the CRTs it may read it through
void SetRuntimeEnv(const char* name, const char* value);
