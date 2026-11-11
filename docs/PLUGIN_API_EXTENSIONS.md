# Plugin API extensions

Extensions to OptiScaler's plugin API (`src/plugin_sdk/optiscaler_plugin.h`, host side `src/OptiScaler/plugins/PluginApi.*`), prototyped in this fork for the lmxxf Neural Rendering plugin (`plugins/lmxxf_nr/`). Each one is a separate commit and is written so its section can become an upstream PR description.

Versioning for all of them: the new functions are appended to the end of `OptiPluginApi`, nothing is moved or removed, so a plugin built against the old header keeps working and a new plugin checks every new field with `OPTI_API_HAS(api, Field)` before calling it. Structs passed to callbacks start with `uint32_t size` (the host's `sizeof`) and only grow at the end. `OPTI_PLUGIN_API_VERSION` stays 1 here; whether a merged extension bumps it is the maintainers' call (the `OPTI_API_HAS` checks don't depend on it).

| # | Extension | Fields | Status |
|---|---|---|---|
| 1 | Upscale callbacks + command list state restore | `RegisterUpscaleCallbacks`, `RestoreCommandListState` | built, used by the plugin |
| 3 | Overlay text lines | `RegisterOverlayText` | built, used by the plugin |
| 2 | Present callback + HUD-less colour without frame generation | `RegisterPresentCallback` | built, used by the plugin |
| 5 | Adapter override + unhooked device creation | `RegisterAdapterOverride`, `CreateD3D12DeviceUnhooked`, `CreateDxgiFactoryUnhooked`, `RunUnspoofed` | built, used by the plugin's multi-GPU mode |
| 6 | Swapchain provider | `RegisterSwapchainProvider`, `CreateSwapChainForHwndUnhooked` | built, used by the plugin's multi-GPU mode |
| 7 | FFX API for plugins (D3D12) | `FfxCreateContextDx12`, `FfxDestroyContextDx12`, `FfxConfigureDx12`, `FfxQueryDx12`, `FfxDispatchDx12` | spike (`spike/displayfg`), used by the plugin's display-card frame generation |

## 1. Upscale callbacks and command list state restore

**Why.** A post-process that wants the upscaler's inputs (colour, depth, motion vectors, exposure, jitter) or its output today has to detour OptiScaler's own `NVSDK_NGX_D3D12_EvaluateFeature` export and decode the NGX parameter block itself. That works only for D3D12 inputs, sees OptiScaler's internal evaluates as well, and cannot tell which resource states OptiScaler assumes. It also can't restore the game's root signatures after its own passes: OptiScaler's tracker is the only one that knows them. Neural Rendering, sharpeners, debug views and capture tools all want the same thing.

**ABI.**

```c
typedef struct OptiUpscaleFrame {
    uint32_t size; uint32_t api;                        // OptiApi_D3D12
    void* device; void* commandList; void* commandQueue; // ID3D12Device*, the game's open list, the present queue
    void* color; void* output; void* depth; void* motionVectors; void* exposure; // ID3D12Resource*, NULL if absent
    uint32_t colorState, outputState, depthState, motionVectorsState, exposureState; // D3D12_RESOURCE_STATES
    uint32_t renderWidth, renderHeight, outputWidth, outputHeight;
    float jitterX, jitterY, mvScaleX, mvScaleY, preExposure;
    uint32_t flags;   // OptiUpscale_Reset, DepthInverted, JitteredMV, DisplayResMV, Hdr, AutoExposure,
                      // RayReconstruction, StatesAssumed
    uint64_t frameId;
    void* colorOverride; // before callbacks: replaces the upscaler's colour for this evaluate
} OptiUpscaleFrame;
typedef void (*OptiUpscaleFn)(OptiUpscaleFrame* frame, void* userData);

bool (*RegisterUpscaleCallbacks)(OptiPlugin* plugin, OptiUpscaleFn before, OptiUpscaleFn after, void* userData);
void (*RestoreCommandListState)(void* commandList);
```

**Behaviour.**

- Called in `TryEvaluateOptiFeature` (`inputs/NVNGX_DLSS_Dx12.cpp`), which every D3D12 input path reaches (DLSS, DLSS-D, FSR 2/3, FFX API, XeSS inputs), whatever the output upscaler. `before` runs right before `feature->Evaluate`, after the frame-generation inputs were taken, so FG sees the game's resources. `after` runs right after a successful evaluate and before `UpscaleEnd`, so resource tracking and HUDfix see the modified output.
- Both run inside OptiScaler's "upscaler active" window, where its root signature tracking is off: the plugin's `SetComputeRootSignature` / `SetDescriptorHeaps` are not mistaken for the game's, and OptiScaler's existing restore (when `RestoreComputeSignature` / `RestoreGraphicSignature` is on for the game) runs after the `after` callbacks. `RestoreCommandListState` lets a plugin trigger the same restore itself, for example before it returns from `before` with the game's state needed; it does nothing outside a callback.
- States: the `*State` fields are the states the game left the resources in as OptiScaler knows them (the `[Hotfix]` `*ResourceBarrier` keys), otherwise the NGX convention its upscalers assume (inputs `NON_PIXEL_SHADER_RESOURCE`, output `UNORDERED_ACCESS`); `OptiUpscale_StatesAssumed` says which. The plugin returns every resource in that state.
- `colorOverride`: OptiScaler sets it as `NVSDK_NGX_Parameter_Color` for the evaluate and puts the game's colour back right after, so nothing outside the evaluate sees it. The resource has to be in `colorState` and at least render size.
- Every callback is wrapped in SEH like the tab draw callbacks: a crash disables that plugin's callbacks, not the game. Registration from `OptiPluginInit` is kept only when it returns true.

**Not covered yet.** The D3D11 and Vulkan inputs (`IFeature_Dx11wDx12`, `IFeature_VkwDx12`) evaluate through their own D3D12 bridge; the same two calls fit around their `Evaluate` with the bridge's D3D12 list and copies. Not done in this prototype.

**What the plugin taught.**

- The game's queue is needed (a runtime that submits work after the game's list, here lmxxf's `EnqueueHip`), so `commandQueue` is in the frame. It is the queue the swapchain was created with; the plugin still detours `ExecuteCommandLists` to learn when the recorded list is submitted. A host-side "list submitted" notification would remove that last self-hook and is the natural follow-up.
- Neural Rendering cannot finish inside one evaluate: its input has to be submitted before the network can run, so the result is applied to the next frame. The callback API doesn't need to know; it is a plugin-side pipeline (see `plugins/lmxxf_nr/README.md`).

## 3. Overlay text lines

**Why.** A plugin that runs per frame work (a neural pass, a second GPU) has numbers the player wants to see while playing, next to the frame time: the pass's GPU time, its size, a fallback state. With API v1 they can only live in the menu tab, which is closed while playing.

**ABI.**

```c
typedef bool (*OptiOverlayTextFn)(char* buffer, size_t bufferSize, void* userData);
bool (*RegisterOverlayText)(OptiPlugin* plugin, const char* label, OptiOverlayTextFn fn, void* userData);
```

**Behaviour.**

- One line per registration, "label: value", drawn by the FPS overlay below OptiScaler's own rows, with the overlay's fonts, colours, label / value styles and digit padding. Shown in the Detailed type and fuller ones and in Custom; the JustFPS and Simple types stay as they are.
- The callback fills `buffer` (256 bytes) and returns false to hide its line for that frame (for example while the plugin's feature is off). It runs on the render thread while the overlay draws, under SEH: a crash removes that line.
- Registrations made in `OptiPluginInit` are dropped when it returns false.

**What the plugin taught.** A text callback is enough for the NR and multi-GPU stats; graphs (frame time style) would need `PlotLines` (assessment PR 4) and are left out. A Custom-overlay row key per plugin line (so users can place them) is a possible follow-up.

## 2. Present callback and the HUD-less colour without frame generation

**Why.** Work that belongs at the end of the frame (Neural Rendering on the finished picture with the UI kept as drawn, a capture tool, a second GPU that presents) needs the back buffer at present time, and most of it needs the frame before its UI. OptiScaler already finds that HUD-less colour (HUDfix), but only feeds it to its own frame generation, so with FG off it is not found at all, and there is no point where a plugin runs at present.

**ABI.**

```c
typedef struct OptiPresentFrame {
    uint32_t size; uint32_t api; uint32_t phase;      // OptiPresent_BeforeOverlay / OptiPresent_AfterOverlay
    uint32_t reserved;
    void* device; void* queue; void* swapchain;       // ID3D12Device*, the present queue, the IDXGISwapChain*
    void* backBuffer; uint32_t backBufferState;       // this frame's buffer, with its UI, PRESENT
    uint32_t colorSpace;                              // DXGI_COLOR_SPACE_TYPE of the output
    void* hudless; uint32_t hudlessState;             // HUDfix's capture of this frame, NULL when none
    uint32_t width, height, format, syncInterval, presentFlags;
    uint64_t frameId, upscaleFrameId;
    uint32_t hudlessSource;    // OptiHudless_None / _Hudfix / _FfxFG (appended)
    uint32_t frameGeneration;  // OptiFrameGen_Off / _Game (appended)
    uint32_t fgSwapchain;      // 1: the game's Present on its own FG swapchain (appended)
} OptiPresentFrame;
typedef void (*OptiPresentFn)(OptiPresentFrame* frame, void* userData);
bool (*RegisterPresentCallback)(OptiPlugin* plugin, OptiPresentFn fn, uint32_t requests, void* userData);
// requests: OptiPresentRequest_Hudless, OptiPresentRequest_PreOverlay
```

**Behaviour.**

- Called from `LocalPresent` (`wrapped/wrapped_swapchain.cpp`) for D3D12 swapchains while frame generation is not presenting: once before OptiScaler draws its menu and FPS overlay (`BeforeOverlay`, where a post-process belongs, so OptiScaler's own UI is not processed), once right before the real `Present` (`AfterOverlay`, where a capture or a second presenter belongs). The plugin submits its own command lists to `queue`; it returns the back buffer in `PRESENT`.
- `OptiPresentRequest_PreOverlay` adds a third call, `OptiPresent_PreOverlay`, on the swapchain OptiScaler draws its menu on, right before it draws. It is also sent when the `BeforeOverlay` / `AfterOverlay` pair ran earlier at the game's FG swapchain (then only the device, queue, swapchain and back buffer are filled). For a plugin that presents the frame elsewhere and must keep the menu out of its processing (lmxxf_nr's multi-GPU whole-frame fallback).
- At the game's FG swapchain (`RunFgSwapchainCallbacks`), a frame whose FG gets no `HUDLessColor` and does not generate takes HUDfix's pick of that frame as `hudless` (`OptiHudless_Hudfix`), as the wrapped swapchain's present does. A game that always creates the FFX swapchain (Hellraiser) had no HUD-less colour for plugins before.
- `OptiPresentRequest_Hudless` keeps HUDfix's HUD-less detection running with frame generation off and whatever `FGInput` is: the resource tracker and its command list hooks are installed at device creation when a plugin asked for it (so it must be asked for in `OptiPluginInit`; the command list hooks were tied to `FGInput=upscaler` before), `Hudfix_Dx12` takes a plugin as an active consumer and tracks bindings the whole frame, the upscale end opens the pick window for the frame like FG does (one pick per upscaled frame), and the present clears the frame's candidates and advances the counter, as `FGPresent` does. The pick is HUDfix's capture buffer as it is (same format group as the swapchain; FG's format conversion needs the FG feature and is skipped), handed over only in the present of the frame it was taken in (`hudless` is NULL otherwise), so a back buffer is never paired with another frame's HUD-less colour.
- When OptiScaler's own FG is active nothing changes: FG keeps the HUD-less pick and the callbacks are not called (FG presents interpolated frames through its own swapchain).
- The game's own frame generation, which presents through OptiScaler's swapchain (FSR FG through the FFX API with OptiScaler's FG off, or DLSS-G through the NGX replacement): `ffxConfigure` with `ffxConfigureDescFrameGeneration` (`FfxApi_Dx12.cpp`) and the DLSS-G evaluate (`Nvngx_FG.cpp`) note the game's HUD-less colour and whether it generates. FG off: the game's `HUDLessColor` goes to the next present as `hudless` (`hudlessSource = OptiHudless_FfxFG`) when HUDfix found none; it is used on the game's queue at that present, before the game's next frame. FG on: `frameGeneration = OptiFrameGen_Game` and no `hudless`: the FFX swapchain presents generated and real frames from its own thread, so a present is not paired with the HUD-less colour and can't be told real from generated here. The lmxxf plugin then runs whole-frame NR on every present (generated and real alike, so the two don't alternate between NR and no NR).
- The game's FG swapchain itself (FSR FG through the FFX API, OptiScaler's FG off): when `ffxCreateContext` makes a `FRAMEGENERATIONSWAPCHAIN_WRAP / NEW / FOR_HWND_DX12` context and a plugin has a present callback, its `Present` / `Present1` are detoured. The callbacks then run at the game's own Present on that swapchain, on the game queue it was created with, before FFX reads the frame: `backBuffer` is the FFX swapchain's current buffer, `hudless` this frame's `HUDLessColor` (in its configured state), `fgSwapchain = 1`, `frameGeneration` says whether it generates. What a plugin writes into both reaches the generated frames, which FFX interpolates from them. The presents the FFX swapchain makes on OptiScaler's swapchain (real and generated) get no callbacks while the FG swapchain presents (500 ms).
- SEH guarded like the other callbacks.

**What the plugin taught.**

- The first version found no HUD-less colour at all in Satisfactory: HUDfix's command list hooks are attached only for `FGInput=upscaler`, and binds were tracked only while the pick window was open. Both are part of the request now.
- Two phases were needed: the terminal NR wants to run before OptiScaler's overlay, the multi-GPU capture after it.
- Pairing matters: the multi-GPU prototype took the pick when HUDfix found it, while the game was already recording the next frame, and paired back buffers with the wrong frame's HUD-less colour (74.9 % to 100 % of pixels "differing" in Satisfactory, up to 209 gate trips per run). Handing the pick over at the present of the frame it was captured in removes that by construction.
- Depth and motion vectors at present time are not included: they are the upscaler's inputs, which the upscale callbacks (1) already give, on the game's command list, in the frame they belong to.
- CONTROL (FSR 3.1/4 through the FFX API, no Streamline DLSS-G on AMD): HUDfix picks nothing, but the game hands its FSR FG a HUD-less colour (`R10G10B10A2` 3840x2160, same format as the back buffer, `state C0`), also with FG on.
- Not covered: D3D11 swapchains, OptiScaler's own FG on, and the game's FG on with the HUD-less colour (that needs NR at the game's `Present` on the FFX swapchain, before FFX takes the back buffer and the HUD-less colour, so generated frames are made from NR'd inputs).

## 5. Adapter override and unhooked device creation

**Why.** A plugin that moves the game to another GPU (render on one card, present on the other) has to decide the adapter where OptiScaler's `D3D12CreateDevice` hook runs: a detour of its own outside it would make OptiScaler capture whatever device the plugin creates for itself, and OptiScaler drops a device created on a card that is not the primary GPU (`nonPrimaryGpu`), which is exactly the render card. The plugin also needs devices and DXGI factories of its own that OptiScaler neither captures, redirects nor spoofs.

**ABI.**

```c
typedef void* (*OptiAdapterOverrideFn)(void* requestedAdapter, void* caller, void* userData);
bool    (*RegisterAdapterOverride)(OptiPlugin* plugin, OptiAdapterOverrideFn fn, void* userData);
int32_t (*CreateD3D12DeviceUnhooked)(void* adapter, int32_t minimumFeatureLevel, const void* riid, void** device);
int32_t (*CreateDxgiFactoryUnhooked)(const void* riid, void** factory);
void    (*RunUnspoofed)(void (*fn)(void* userData), void* userData);
```

**Behaviour.**

- `RegisterAdapterOverride`: one per process (the first plugin keeps it), registered in `OptiPluginInit`. Called at the top of `hkD3D12CreateDevice` and of the `ID3D12DeviceFactory::CreateDevice` hook, for every creation except feature-level-1_0_CORE probes and unhooked ones, with the caller's return address so the plugin can leave DXGI's, D3D12's and OptiScaler's own devices alone. A returned adapter replaces the requested one (the plugin keeps it alive), and the device is then the game's device whatever GPU it is on: the `nonPrimaryGpu` rejection is skipped for it.
- `CreateD3D12DeviceUnhooked`: the real `D3D12CreateDevice` through a thread-local pass-through in both hooks: no override, no capture as the game's device.
- `CreateDxgiFactoryUnhooked`: the system DXGI's `CreateDXGIFactory1` (OptiScaler's proxy original), no factory wrapping.
- `RunUnspoofed`: runs a function with OptiScaler's GPU spoofing off for the thread (`ScopedSkipSpoofingThread`), for adapter enumeration with real vendor ids and names.
- SEH guarded; a crashing override is removed.

**What the plugin taught.** The caller filter (DXGI, D3D12, D3D12Core, D3D11, DXCore, OptiScaler, the plugin) is necessary: redirecting DXGI's own internal device hung the cross-adapter present in the multi-GPU prototype. It is passed to the plugin rather than applied by the host, because which callers matter is the plugin's policy.

## 6. Swapchain provider

**Why.** A plugin that presents on another GPU must create the real swapchain on that GPU's queue and give the game a proxy whose buffers live on the game's GPU, while OptiScaler's wrapper (menu, overlay, present callbacks) stays on top of what the game sees. That point is inside OptiScaler's `CreateSwapChainForHwnd` hook, between deciding whether to build the FG swapchain and calling the real factory; a plugin cannot place itself there from outside.

**ABI.**

```c
typedef int32_t (*OptiSwapchainProviderFn)(void* factory, void* queue, void* hwnd, const void* desc1,
                                           const void* fullscreenDesc, void* restrictToOutput, void** swapChain,
                                           void* userData);
bool    (*RegisterSwapchainProvider)(OptiPlugin* plugin, OptiSwapchainProviderFn fn, void* userData);
int32_t (*CreateSwapChainForHwndUnhooked)(void* factory, void* device, void* hwnd, const void* desc1,
                                          const void* fullscreenDesc, void* restrictToOutput, void** swapChain);
```

**Behaviour.**

- One provider per process, registered in `OptiPluginInit`. In `DxgiFactoryHooks::CreateSwapChainForHwnd` and the legacy `IDXGIFactory::CreateSwapChain` (hooked, DLSSG-hooked and wrapped-factory paths), for a D3D12 queue: with a provider registered OptiScaler skips its FG swapchain and calls the provider where it would call the real factory, with its adjusted descriptions (V-sync override, tearing flag). `S_OK` + a swapchain: OptiScaler wraps it as `WrappedIDXGISwapChain4` exactly like a real one; `E_NOTIMPL`: it creates the swapchain itself, as before.
- Legacy `CreateSwapChain` reaches the provider in the same `desc1` + fullscreen desc + `hwnd` form: `BufferDesc` size/format, `SampleDesc`, `BufferUsage`, `BufferCount`, `SwapEffect` and `Flags` go to `desc1` (`Scaling` = `DXGI_SCALING_STRETCH`, `AlphaMode` unspecified); `RefreshRate`, `ScanlineOrdering`, `BufferDesc.Scaling` and `Windowed` go to the fullscreen desc, which is always passed; `hwnd` is `OutputWindow`. The provider's swapchain goes back to the game QI'd to `IDXGISwapChain`.
- `CreateSwapChainForCoreWindow` and `CreateSwapChainForComposition` are not covered: with a provider registered they log a warning and create the swapchain without it.
- `CreateSwapChainForHwndUnhooked` is the factory's real `CreateSwapChainForHwnd` (OptiScaler's saved original, no parent wrapping), for the provider's own real swapchain on another device.
- SEH guarded; a crashing provider is removed and OptiScaler creates the swapchain.

**What the plugin taught.**

- Registering a provider turns OptiScaler's FG swapchain off for that process even when the provider then declines; FG output and a plugin presenter are exclusive anyway (the multi-GPU design suppresses FG output), and deciding later would mean building the FG swapchain first and discarding it.
- CONTROL creates its swapchain with the legacy `CreateSwapChain`: the device went to the render card but no proxy was made until that path called the provider too. CoreWindow and composition swapchains have no `HWND` for the provider's real swapchain; they stay uncovered until a game needs them.

## 7. FFX API for plugins (D3D12)

**Why.** A plugin that presents on its own swapchain (the multi-GPU presenter) can only get frame generation by running it itself. OptiScaler already loads, versions and routes the FidelityFX modules (FSR 3.1 / 4 FG, the FSR 4 driver provider on RDNA 4); a plugin that loads them again meets OptiScaler's `LoadLibrary` and `GetModuleHandle` redirection and the game's own hooked copies.

**ABI.**

```c
uint32_t (*FfxCreateContextDx12)(void** context, void* desc, const void* memCb);
uint32_t (*FfxDestroyContextDx12)(void** context, const void* memCb);
uint32_t (*FfxConfigureDx12)(void** context, const void* desc);
uint32_t (*FfxQueryDx12)(void** context, void* desc);
uint32_t (*FfxDispatchDx12)(void** context, const void* desc);
```

**Behaviour.**

- They are `ffxCreateContext` / `ffxDestroyContext` / `ffxConfigure` / `ffxQuery` / `ffxDispatch` of `FfxApiProxy`'s own modules (never the game's, never hooked for inputs); descs, contexts and return codes are the FFX API's (`ffx_api.h`, MIT). The first call loads the modules if OptiScaler has not yet.
- A frame generation swapchain context (`FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12`) created through `FfxCreateContextDx12` gets a plain DXGI swapchain: on that thread the `CreateSwapChainForHwnd` hook passes the call through (no swapchain provider, no `WrappedIDXGISwapChain4`, no `State` capture), so the FFX swapchain can sit under a plugin's proxy without OptiScaler taking it for the game's.

**What the plugin taught.** On the display card of the multi-GPU mode the FG swapchain and context (provider 4.0.2 from OptiScaler's `amd_fidelityfx_framegeneration_dx12.dll`) are created on the plugin's own device and queue; nothing else of OptiScaler's FG is involved. The `CreateSwapChainForHwnd` pass-through was the only host change it needed.

