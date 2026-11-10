// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Legacy mode, game side: the in-game menu and toasts (Dear ImGui, DX12 backend) drawn on the back buffer at present.
#pragma once
#include <windows.h>
#include <d3d12.h>

#include <cstdint>

#include "../LegacyShared.h"

namespace Overlay
{
struct Settings
{
    bool nr = true; // false: frames skip the host (bypass)
    Legacy::Control c {};
};

struct Readouts
{
    float fps = 0, frameMs = 0; // shown on the screen (multi-GPU: the presenter's)
    float gameFps = 0, latencyMs = 0; // the game's presents; its present call -> real present
    float hostMs = 0, netMs = 0, ofMs = 0, waitMs = 0, proxyWaitMs = 0;
    uint32_t feedW = 0, feedH = 0, outW = 0, outH = 0;
    uint64_t presents = 0, timeouts = 0, skipped = 0, bypassed = 0, failedPresents = 0, nrFrames = 0, passFrames = 0;
    uint64_t dropped = 0, occluded = 0; // multi-GPU: game frames never sent (ring full), frames not presented
    bool mgpu = false, mgpuOff = false;
    int hostState = 0;
    bool hostUp = false, applied = true, on12 = false;
    uint32_t caps = 0;
    float flkStatic = 0, flkMc = 0;
    char status[192] = {}, error[128] = {};
};

// Proxy.cpp
Settings GetSettings();
void SetSettings(const Settings& s); // pushes to the host, saves the ini shortly after
Readouts GetReadouts();

// Overlay.cpp
bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format, uint32_t height);
void Shutdown();
void Toast(const char* fmt, ...);
bool MenuOpen();
void SetMenu(bool open);
bool Visible(); // menu open or a toast showing
// Records the overlay onto target (RENDER_TARGET state) on list
void Draw(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w,
          uint32_t h);
// Input while the menu is open (window thread); true: the game must not see the message
bool OnMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
} // namespace Overlay
