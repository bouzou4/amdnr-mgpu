/*
 * OptiScaler plugin API
 *
 * Lets an ASI plugin loaded by OptiScaler (plugins folder, LoadAsiPlugins=true) add its own tabs to the
 * OptiScaler menu and keep its settings in OptiScaler.ini.
 *
 * The API is plain C and does not depend on the ImGui version used by OptiScaler, so a plugin built against
 * this header keeps working with newer OptiScaler builds. No OptiScaler library or ImGui is linked.
 *
 * Usage:
 *   Export OptiPluginInit from the plugin. OptiScaler calls it once, right after InitializeASI (if present).
 *   Custom upscaler and FG dlls (custom/<name>/<kind>) can export it too, it's called when OptiScaler loads them,
 *   which can be long after the game started.
 *   Keep the api pointer, it stays valid for the lifetime of the process.
 *
 * Compatibility rules:
 *   - Fields are only ever appended to OptiPluginApi, never removed or reordered.
 *   - api->size is sizeof(OptiPluginApi) of the running OptiScaler. Before using a field added after
 *     version 1, check it with OPTI_API_HAS(api, Field).
 *
 * Strings are UTF-8 and copied by OptiScaler when it has to keep them.
 *
 * Threading:
 *   - Widget functions only work inside a tab draw callback, which runs on the render thread while the menu
 *     is open. Outside of it they do nothing and return false.
 *   - Draw callbacks have to be quick and must not call into D3D/Vulkan or present.
 *   - Registration, settings and logging functions can be called from any thread.
 *
 * Settings:
 *   Stored in OptiScaler.ini, in the [Plugin.<name>] section. <name> is the plugin file name without the
 *   extension and without a "-loadlate" suffix, or the folder name for a custom dll. A missing key or the value "auto"
 * returns the default. Set* only changes the value in memory, SaveSettings (or Save INI in the menu) writes the file.
 */

#ifndef OPTISCALER_PLUGIN_H
#define OPTISCALER_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define OPTI_PLUGIN_API_VERSION 1

    // Handle of the calling plugin, given to OptiPluginInit
    typedef struct OptiPlugin OptiPlugin;

    // Called every frame while the tab is selected and the menu is open
    typedef void (*OptiDrawTabFn)(void* userData);

    typedef enum OptiLogLevel
    {
        OptiLog_Debug = 0,
        OptiLog_Info = 1,
        OptiLog_Warn = 2,
        OptiLog_Error = 3,
    } OptiLogLevel;

    // Upscale callbacks (RegisterUpscaleCallbacks) -----------------------------------------------------------

    typedef enum OptiGraphicsApi
    {
        OptiApi_D3D12 = 1,
    } OptiGraphicsApi;

    typedef enum OptiUpscaleFlags
    {
        OptiUpscale_Reset = 1 << 0,              // history should be dropped this frame
        OptiUpscale_DepthInverted = 1 << 1,      // 1 = near, 0 = far
        OptiUpscale_JitteredMV = 1 << 2,         // motion vectors include the jitter
        OptiUpscale_DisplayResMV = 1 << 3,       // motion vectors are at output size
        OptiUpscale_Hdr = 1 << 4,                // color is linear HDR
        OptiUpscale_AutoExposure = 1 << 5,       // no exposure texture, the upscaler computes it
        OptiUpscale_RayReconstruction = 1 << 6,  // the evaluate also denoises (DLSS-D / FSR Ray Regeneration)
        OptiUpscale_StatesAssumed = 1 << 7,      // *State fields are the API's convention, not set by the game config
    } OptiUpscaleFlags;

    // One upscaler evaluate. Resources are ID3D12Resource*, NULL when the game doesn't provide them.
    // The plugin records into commandList and must leave every resource in its *State when it returns.
    // Descriptor heaps, root signatures and pipeline state may be changed, like the upscaler itself does; the game
    // rebinds after an evaluate (OptiScaler restores them after the callbacks when root signature restore is on).
    typedef struct OptiUpscaleFrame
    {
        uint32_t size; // sizeof(OptiUpscaleFrame) of the running OptiScaler, fields are only appended
        uint32_t api;  // OptiGraphicsApi
        void* device;       // ID3D12Device*
        void* commandList;  // ID3D12GraphicsCommandList* of the game, open
        void* commandQueue; // ID3D12CommandQueue* the game presents with, NULL when not known yet
        void* color;
        void* output;
        void* depth;
        void* motionVectors;
        void* exposure;
        uint32_t colorState; // D3D12_RESOURCE_STATES
        uint32_t outputState;
        uint32_t depthState;
        uint32_t motionVectorsState;
        uint32_t exposureState;
        uint32_t renderWidth; // part of color the game rendered to
        uint32_t renderHeight;
        uint32_t outputWidth;
        uint32_t outputHeight;
        float jitterX; // in render pixels
        float jitterY;
        float mvScaleX; // motion vector * scale = offset in render pixels to the previous frame
        float mvScaleY;
        float preExposure;
        uint32_t flags; // OptiUpscaleFlags
        uint64_t frameId; // increments once per evaluate
        // Before callbacks only: set to an ID3D12Resource* (render size or larger, colorState, shader readable) to
        // use it as the upscaler's color for this evaluate. The last plugin that sets it wins.
        void* colorOverride;
    } OptiUpscaleFrame;

    // before runs ahead of the upscaler, after runs once it has recorded into frame->commandList
    typedef void (*OptiUpscaleFn)(OptiUpscaleFrame* frame, void* userData);

    // Overlay text (RegisterOverlayText) -------------------------------------------------------------------

    // Writes the value shown after the line's label into buffer (zero terminated, truncated to bufferSize).
    // Return false to hide the line this frame. Runs on the render thread while the overlay draws: keep it quick,
    // no D3D / Vulkan calls.
    typedef bool (*OptiOverlayTextFn)(char* buffer, size_t bufferSize, void* userData);

    // Present callbacks (RegisterPresentCallback) -------------------------------------------------------------

    typedef enum OptiPresentPhase
    {
        OptiPresent_BeforeOverlay = 0, // OptiScaler's menu and FPS overlay are not drawn yet
        OptiPresent_AfterOverlay = 1,  // right before the swapchain Present
    } OptiPresentPhase;

    typedef enum OptiPresentRequest
    {
        // Capture the HUD-less colour (HUDfix's detection) even without frame generation. Ask in OptiPluginInit:
        // the resource tracking it needs is set up when the game creates its device.
        OptiPresentRequest_Hudless = 1 << 0,
    } OptiPresentRequest;

    // One D3D12 present of the game's swapchain (frame generation off). The plugin submits its own lists to queue
    // and leaves backBuffer in backBufferState and hudless in hudlessState.
    typedef struct OptiPresentFrame
    {
        uint32_t size; // sizeof(OptiPresentFrame) of the running OptiScaler, fields are only appended
        uint32_t api;  // OptiGraphicsApi
        uint32_t phase; // OptiPresentPhase
        uint32_t reserved;
        void* device;    // ID3D12Device*
        void* queue;     // ID3D12CommandQueue* the swapchain presents with
        void* swapchain; // IDXGISwapChain* OptiScaler presents
        void* backBuffer; // ID3D12Resource* the game rendered this frame into, with its UI
        uint32_t backBufferState; // D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATE_PRESENT
        uint32_t colorSpace;      // DXGI_COLOR_SPACE_TYPE the swapchain output uses
        void* hudless; // ID3D12Resource*, the frame before its UI, NULL when not captured this frame
        uint32_t hudlessState;
        uint32_t width; // back buffer size and DXGI_FORMAT
        uint32_t height;
        uint32_t format;
        uint32_t syncInterval;
        uint32_t presentFlags;
        uint64_t frameId;        // increments once per present
        uint64_t upscaleFrameId; // OptiUpscaleFrame::frameId of the last evaluate, 0 when none
    } OptiPresentFrame;

    typedef void (*OptiPresentFn)(OptiPresentFrame* frame, void* userData);

    // Adapters and devices (RegisterAdapterOverride) -------------------------------------------------------

    // Called in OptiScaler's D3D12CreateDevice hook for every device creation except OptiScaler's own and the
    // plugins' unhooked ones. requestedAdapter is the caller's IUnknown* adapter (may be NULL = default), caller
    // the return address of the call. Return an IUnknown* adapter to create the device on instead (the plugin keeps
    // it alive), or NULL to keep the request. An overridden device counts as the game's device, on any GPU.
    typedef void* (*OptiAdapterOverrideFn)(void* requestedAdapter, void* caller, void* userData);

    // Swapchains (RegisterSwapchainProvider) --------------------------------------------------------------------

    // Called where OptiScaler creates the game's D3D12 swapchain (CreateSwapChainForHwnd), instead of its frame
    // generation swapchain. Return S_OK with *swapChain set (an IDXGISwapChain1*, one reference for OptiScaler)
    // to provide it: OptiScaler wraps it like a real one (menu, overlay, present callbacks). Return E_NOTIMPL to
    // let OptiScaler create it as usual. factory, queue: IDXGIFactory2*, the game's ID3D12CommandQueue*.
    typedef int32_t (*OptiSwapchainProviderFn)(void* factory, void* queue, void* hwnd, const void* desc1,
                                               const void* fullscreenDesc, void* restrictToOutput, void** swapChain,
                                               void* userData);

    typedef struct OptiPluginApi
    {
        uint32_t size;    // sizeof(OptiPluginApi) of the running OptiScaler
        uint32_t version; // OPTI_PLUGIN_API_VERSION of the running OptiScaler

        // Registration -------------------------------------------------------------------------------------

        // Adds a tab under the PLUGINS group of the menu sidebar. A plugin can register several tabs.
        // description is shown as the sidebar tooltip and can be NULL.
        bool (*RegisterTab)(OptiPlugin* plugin, const char* name, const char* description, OptiDrawTabFn draw,
                            void* userData);

        // Layout, only inside a draw callback ---------------------------------------------------------------

        // Starts a new card with a title, the previous one is closed. Cards are closed after the callback.
        void (*Section)(const char* title);
        void (*Separator)(void);
        // Places the next widget on the same line as the previous one
        void (*SameLine)(void);
        void (*Spacing)(void);
        // Widgets after Disabled(true) can't be interacted with, until Disabled(false) or the end of the callback
        void (*Disabled)(bool disabled);
        // Shows a tooltip when the previous widget is hovered
        void (*ItemTooltip)(const char* text);
        // True on the frame the previous widget stopped being edited, a slider was released for example.
        // Lets a plugin apply a value once instead of on every frame of a drag.
        bool (*ItemDeactivatedAfterEdit)(void);

        // Text, only inside a draw callback -----------------------------------------------------------------

        void (*Text)(const char* text);
        void (*TextDisabled)(const char* text);
        // r, g, b, a in 0.0 - 1.0
        void (*TextColored)(float r, float g, float b, float a, const char* text);

        // Widgets, only inside a draw callback --------------------------------------------------------------
        // Labels have to be unique inside a tab. Text after "##" is not shown, "Scale##2" is drawn as "Scale".
        // format is printf-like for the value shown on the slider, NULL for the default.

        bool (*Button)(const char* label);                // true when clicked
        bool (*Checkbox)(const char* label, bool* value); // true when changed
        bool (*SliderFloat)(const char* label, float* value, float min, float max, const char* format);
        bool (*SliderInt)(const char* label, int32_t* value, int32_t min, int32_t max, const char* format);
        // items is an array of itemCount strings, current is the selected index.
        // itemDisabled is NULL or an array of itemCount bools, true greys out the item. The selected item is
        // shown even when it's disabled.
        bool (*Combo)(const char* label, int32_t* current, const char* const* items, int32_t itemCount,
                      const bool* itemDisabled);
        // buffer is a zero terminated UTF-8 string that is edited in place
        bool (*InputText)(const char* label, char* buffer, size_t bufferSize);
        // rgba is float[4] in 0.0 - 1.0
        bool (*ColorEdit)(const char* label, float* rgba, bool withAlpha);

        // Menu state ----------------------------------------------------------------------------------------

        bool (*IsMenuOpen)(void);

        // Settings in [Plugin.<name>] of OptiScaler.ini ---------------------------------------------------

        bool (*GetBool)(OptiPlugin* plugin, const char* key, bool defaultValue);
        int32_t (*GetInt)(OptiPlugin* plugin, const char* key, int32_t defaultValue);
        float (*GetFloat)(OptiPlugin* plugin, const char* key, float defaultValue);
        // Copies the value to buffer (always zero terminated, truncated if needed).
        // Returns false and copies defaultValue when the key is missing.
        bool (*GetString)(OptiPlugin* plugin, const char* key, char* buffer, size_t bufferSize,
                          const char* defaultValue);

        void (*SetBool)(OptiPlugin* plugin, const char* key, bool value);
        void (*SetInt)(OptiPlugin* plugin, const char* key, int32_t value);
        void (*SetFloat)(OptiPlugin* plugin, const char* key, float value);
        void (*SetString)(OptiPlugin* plugin, const char* key, const char* value);
        // Removes the key, Get* returns the default afterwards
        void (*ResetValue)(OptiPlugin* plugin, const char* key);

        // Writes OptiScaler.ini to the disk
        bool (*SaveSettings)(OptiPlugin* plugin);

        // Misc ------------------------------------------------------------------------------------------------

        // Writes to the OptiScaler log, prefixed with the plugin name
        void (*Log)(OptiPlugin* plugin, OptiLogLevel level, const char* message);

        // Upscale callbacks, added after version 1 (check with OPTI_API_HAS) -------------------------------------

        // Calls before / after (either can be NULL) on the render thread around every upscaler evaluate,
        // D3D12 for now (native D3D12 inputs). One registration per plugin, a second one replaces the first.
        bool (*RegisterUpscaleCallbacks)(OptiPlugin* plugin, OptiUpscaleFn before, OptiUpscaleFn after, void* userData);
        // Rebinds the root signatures, root arguments and (with ExtendedStateRestore) descriptor heaps and pipeline
        // state the game had set on commandList before the evaluate. Does nothing when the game config has root
        // signature restore off, or outside of an upscale callback.
        void (*RestoreCommandListState)(void* commandList);

        // Overlay text, added after version 1 (check with OPTI_API_HAS) ---------------------------------------

        // Adds a line "label: value" to the FPS overlay, below OptiScaler's rows, in the Detailed and fuller
        // overlay types and in Custom. One line per call, in registration order.
        bool (*RegisterOverlayText)(OptiPlugin* plugin, const char* label, OptiOverlayTextFn fn, void* userData);

        // Present callback, added after version 1 (check with OPTI_API_HAS) --------------------------------------

        // Calls fn twice per D3D12 present on the presenting thread, phase BeforeOverlay then AfterOverlay.
        // requests: OptiPresentRequest bits. One registration per plugin, a second one replaces the first.
        bool (*RegisterPresentCallback)(OptiPlugin* plugin, OptiPresentFn fn, uint32_t requests, void* userData);

        // Adapters and devices, added after version 1 (check with OPTI_API_HAS) --------------------------------

        // One override per process; register in OptiPluginInit, before the game creates its device.
        bool (*RegisterAdapterOverride)(OptiPlugin* plugin, OptiAdapterOverrideFn fn, void* userData);
        // D3D12CreateDevice that OptiScaler neither redirects nor takes as the game's device (HRESULT)
        int32_t (*CreateD3D12DeviceUnhooked)(void* adapter, int32_t minimumFeatureLevel, const void* riid,
                                             void** device);
        // CreateDXGIFactory1 of the system DXGI, without OptiScaler's spoofing and wrapping (HRESULT)
        int32_t (*CreateDxgiFactoryUnhooked)(const void* riid, void** factory);
        // Runs fn on this thread with OptiScaler's GPU spoofing off (real vendor ids and adapter names)
        void (*RunUnspoofed)(void (*fn)(void* userData), void* userData);

        // Swapchains, added after version 1 (check with OPTI_API_HAS) -------------------------------------------

        // One provider per process; register in OptiPluginInit.
        bool (*RegisterSwapchainProvider)(OptiPlugin* plugin, OptiSwapchainProviderFn fn, void* userData);
        // The factory's real CreateSwapChainForHwnd, without OptiScaler's wrapping (HRESULT). For providers.
        int32_t (*CreateSwapChainForHwndUnhooked)(void* factory, void* device, void* hwnd, const void* desc1,
                                                  const void* fullscreenDesc, void* restrictToOutput,
                                                  void** swapChain);

    } OptiPluginApi;

// True when the running OptiScaler has the given field of OptiPluginApi
#define OPTI_API_HAS(api, field)                                                                                       \
    ((api) != NULL && (api)->size >= offsetof(OptiPluginApi, field) + sizeof((api)->field) && (api)->field != NULL)

    // Exported by the plugin. Return false to not use the API (nothing registered is kept).
    typedef bool (*PFN_OptiPluginInit)(const OptiPluginApi* api, OptiPlugin* plugin);

#define OPTI_PLUGIN_INIT_NAME "OptiPluginInit"

#ifdef __cplusplus
}
#endif

#endif // OPTISCALER_PLUGIN_H
