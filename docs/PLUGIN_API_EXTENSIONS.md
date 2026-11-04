# Plugin API extensions

Extensions to OptiScaler's plugin API (`src/plugin_sdk/optiscaler_plugin.h`, host side `src/OptiScaler/plugins/PluginApi.*`), prototyped in this fork for the lmxxf Neural Rendering plugin (`plugins/lmxxf_nr/`). Each one is a separate commit and is written so its section can become an upstream PR description.

Versioning for all of them: the new functions are appended to the end of `OptiPluginApi`, nothing is moved or removed, so a plugin built against the old header keeps working and a new plugin checks every new field with `OPTI_API_HAS(api, Field)` before calling it. Structs passed to callbacks start with `uint32_t size` (the host's `sizeof`) and only grow at the end. `OPTI_PLUGIN_API_VERSION` stays 1 here; whether a merged extension bumps it is the maintainers' call (the `OPTI_API_HAS` checks don't depend on it).

| # | Extension | Fields | Status |
|---|---|---|---|
| 1 | Upscale callbacks + command list state restore | `RegisterUpscaleCallbacks`, `RestoreCommandListState` | built, used by the plugin |
| 3 | Overlay text lines | `RegisterOverlayText` | built, used by the plugin |
| 2 | Present callback with the frame-end resources | see 2 | see 2 |
| 5 | Adapter override + unhooked device creation | see 5 | see 5 |
| 6 | Swapchain provider | see 6 | see 6 |

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
