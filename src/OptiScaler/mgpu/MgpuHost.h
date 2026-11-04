// The only multi-GPU header OptiScaler code includes: every hook point calls through here.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

struct NVSDK_NGX_Parameter;
class IFeature_Dx12;

namespace MgpuHost
{
// D3D12CreateDevice hook
bool InternalDeviceCreate();
IUnknown* AdapterForDevice(IUnknown* adapter, void* caller);
bool IsRenderAdapter(const LUID& luid);

// Swapchain hook: true = the display card presents, so the game gets a proxy (no FG swapchain).
bool OwnsSwapchains();
HRESULT CreateSwapChainForHwnd(void* original, IDXGIFactory2* factory, IUnknown* queue, HWND hwnd,
                               const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                               IDXGIOutput* restrict, IDXGISwapChain1** out);

// NR runs on the display card: the render card's NR bridge stays idle.
bool BlockRenderCardNr();

// Upscaler Evaluate done (render card, game's list): motion vectors for the temporal stage, and the HUD-less
// pick armed for this frame.
void UpscaleEvaluated(ID3D12GraphicsCommandList* cl, NVSDK_NGX_Parameter* params, IFeature_Dx12* feature);
// HUDfix hands its pick here instead of to frame generation (Mode=hudless, FG output off).
bool HudlessSink();
void HudlessCaptured(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES state);

// UI
void DrawMenuTab(float scale);
void DrawOverlayLines(bool horizontal);
} // namespace MgpuHost
