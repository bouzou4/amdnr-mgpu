// The only multi-GPU header OptiScaler code includes: every hook point calls through here.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace MgpuHost
{
// D3D12CreateDevice hook
bool InternalDeviceCreate();
IUnknown* AdapterForDevice(IUnknown* adapter);
bool IsRenderAdapter(const LUID& luid);

// Swapchain hook: true = the display card presents, so the game gets a proxy (no FG swapchain).
bool OwnsSwapchains();
HRESULT CreateSwapChainForHwnd(void* original, IDXGIFactory2* factory, IUnknown* queue, HWND hwnd,
                               const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                               IDXGIOutput* restrict, IDXGISwapChain1** out);

// NR runs on the display card: the render card's NR bridge stays idle.
bool BlockRenderCardNr();

// UI
void DrawMenuTab(float scale);
void DrawOverlayLines(bool horizontal);
} // namespace MgpuHost
