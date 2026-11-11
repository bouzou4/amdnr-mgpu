#pragma once

#include "SysUtils.h"

#include "../../plugin_sdk/optiscaler_plugin.h"

#include <imgui/imgui.h>

#include <d3d12.h>
#include <dxgi1_2.h>

class IFeature;
struct NVSDK_NGX_Parameter;

// OptiScaler side of plugin_sdk/optiscaler_plugin.h
class PluginApi
{
  public:
    struct TabInfo
    {
        std::string name;
        std::string description;
    };

    // Calls OptiPluginInit of a loaded ASI plugin, once per module
    static void InitPlugin(HMODULE module, PFN_OptiPluginInit init, const std::filesystem::path& path);

    // Same for any other dll, name is its [Plugin.<name>] section
    static void InitPlugin(HMODULE module, PFN_OptiPluginInit init, const std::string& name);

    static std::vector<TabInfo> GetTabs();

    // Draws a registered tab inside of the menu, colorMap adjusts colors for the HDR menu
    static void DrawTab(size_t index, float menuScale, ImVec4 (*colorMap)(const ImVec4&));

    // Upscale callbacks of the plugins, around a D3D12 evaluate (also the D3D11 / Vulkan bridges' one)
    static bool HasUpscaleCallbacks();
    static void RunUpscaleCallbacks(bool after, OptiUpscaleFrame* frame);
    // Fills frame from an evaluate's NGX parameters, which hold the D3D12 resources the feature evaluates
    static void FillUpscaleFrame(OptiUpscaleFrame& frame, IFeature* feature, NVSDK_NGX_Parameter* params,
                                 ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue);

    struct OverlayLine
    {
        std::string label;
        std::string value;
    };

    // The plugins' overlay lines that are shown this frame
    static std::vector<OverlayLine> GetOverlayLines();

    // Present callbacks of the plugins
    static bool HasPresentCallbacks();
    static bool WantsHudless(); // a plugin asked for the HUD-less capture without frame generation
    static uint64_t LastUpscaleFrameId();
    // HUD-less colour the game hands its own frame generation, and whether it generates frames
    static void SetGameFg(const char* source, ID3D12Resource* hudless, uint32_t hudlessState, bool generating);
    // The game's HUD-less colour for this present (consumed), and whether the game's FG generates frames now
    static ID3D12Resource* TakeGameHudless(uint32_t* state, bool* generating);
    // The same at the game's Present on its own FG swapchain (paired with this frame there, generating or not)
    static ID3D12Resource* PeekGameHudless(uint32_t* state, bool* generating);
    // Present callbacks ran at the game's FG swapchain Present: the presents it makes are not called back again
    static void NoteFgSwapchainPresent();
    static bool FgSwapchainPresenting();
    static void RunPresentCallbacks(OptiPresentFrame* frame);

    // Adapter override and unhooked device creation (D3D12CreateDevice hooks)
    static bool UnhookedDeviceCreate();
    static IUnknown* OverrideAdapter(IUnknown* requested, void* caller);
    // A plugin creates an FFX frame generation swapchain: the factory hooks pass it through
    static bool FfxSwapchainCreate();

    // Swapchain provider (CreateSwapChainForHwnd hook)
    static bool HasSwapchainProvider();
    static HRESULT ProvideSwapchain(IDXGIFactory2* factory, IUnknown* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, IDXGIOutput* restrictToOutput,
                                    IDXGISwapChain1** swapChain);
    // Legacy IDXGIFactory::CreateSwapChain through the same provider; E_NOTIMPL when none or declined
    static HRESULT ProvideSwapchainLegacy(IDXGIFactory* factory, IUnknown* queue, const DXGI_SWAP_CHAIN_DESC* desc,
                                          IDXGISwapChain** swapChain);
};
