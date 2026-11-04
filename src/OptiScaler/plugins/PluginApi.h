#pragma once

#include "SysUtils.h"

#include "../../plugin_sdk/optiscaler_plugin.h"

#include <imgui/imgui.h>

#include <dxgi1_2.h>

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

    // Upscale callbacks of the plugins, around a D3D12 evaluate
    static bool HasUpscaleCallbacks();
    static void RunUpscaleCallbacks(bool after, OptiUpscaleFrame* frame);

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
    static void RunPresentCallbacks(OptiPresentFrame* frame);

    // Adapter override and unhooked device creation (D3D12CreateDevice hooks)
    static bool UnhookedDeviceCreate();
    static IUnknown* OverrideAdapter(IUnknown* requested, void* caller);

    // Swapchain provider (CreateSwapChainForHwnd hook)
    static bool HasSwapchainProvider();
    static HRESULT ProvideSwapchain(IDXGIFactory2* factory, IUnknown* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, IDXGIOutput* restrictToOutput,
                                    IDXGISwapChain1** swapChain);
};
