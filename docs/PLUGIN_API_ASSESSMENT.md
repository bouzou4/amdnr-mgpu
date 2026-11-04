# OptiScaler plugin API: can the NR plugin ship on stock OptiScaler?

Target product: one plugin DLL loaded by **stock upstream OptiScaler**, an "lmxxf NR plugin with configurable multi-GPU". OptiScaler keeps doing everything it does (DLSS/FSR/XeSS inputs and outputs, FG, Ray Regeneration, HUDfix, spoofing). The plugin adds:

1. Neural Rendering through the lmxxf runtime's C API (`LmxxfNrApi.h`, ABI v1), at a chosen placement: before upscaling, after upscaling, or terminal (on the HUD-less image at present time), with looks / presets.
2. Optional multi-GPU: the game on one card, NR and present on the other (`docs/MGPU_DESIGN.md`).

Upstream reviewed: OptiScaler master `5eb4bfab` (2026-11-03), `plugin_sdk/` (README, `optiscaler_plugin.h`, `sample/`), `OptiScaler/plugins/PluginApi.*`, `misc/CustomProviders.h`, `framegen/nvngx/Nvngx_Custom.*`, `Nvngx_DllProxy.cpp`, `proxies/FfxApi_Proxy.h` (custom FFX loading), `upscalers/ffx/FFXFeature_Dx12.cpp`, `inputs/*_Dx12.cpp`, `hooks/D3D12_Hooks.cpp`, `dllmain.cpp` (ASI loading). Commits: `e6a4c3e8` (Plugin api), `9302028e` (ComboEx, ItemDeactivatedAfterEdit), `df8a8f13` (custom upscaler / FG dlls get the API), `de2626f1`, `63463bde`, `760ae412` (custom FFX upscalers DX12 / Vk), `941ce6a1` (custom DLSSG dlls).

## 1. What upstream offers

### 1.1 The plugin API (`OptiPluginApi`, version 1)

A plain C function table, appended-only (`OPTI_API_HAS(api, Field)` for later fields). It is a **UI, settings and log API**, nothing else:

| Group | Functions |
|---|---|
| Registration | `RegisterTab(plugin, name, description, OptiDrawTabFn, userData)`: a tab under PLUGINS in the menu sidebar |
| Layout / text / widgets (only inside a draw callback) | `Section`, `Separator`, `SameLine`, `Spacing`, `Disabled`, `ItemTooltip`, `ItemDeactivatedAfterEdit`, `Text`, `TextDisabled`, `TextColored`, `Button`, `Checkbox`, `SliderFloat`, `SliderInt`, `Combo` (with per-item disable), `InputText`, `ColorEdit` |
| Menu state | `IsMenuOpen` |
| Settings, `[Plugin.<name>]` of `OptiScaler.ini` | `GetBool/Int/Float/String`, `SetBool/Int/Float/String`, `ResetValue`, `SaveSettings` |
| Log | `Log(plugin, level, message)`, prefixed with the plugin name |

Behaviour: draw callbacks run on the render thread while the menu is open, wrapped in SEH (a crash disables the tab, not the game); widgets are no-ops outside a callback; settings are in-memory until `SaveSettings`. There is no D3D object, no resource, no frame event, no overlay hook and no raw ImGui access.

### 1.2 Who gets it

| Kind | Where | When `OptiPluginInit` runs | What the DLL itself receives |
|---|---|---|---|
| ASI plugin | `<OptiScaler folder>/plugins/*.asi`, `[Plugins] LoadAsiPlugins=true` | During OptiScaler's init (`dllmain.cpp` `LoadAsiPlugins`), right after `InitializeASI`; `-loadlate` ones after `LateAsiPluginsDelay` | Nothing beyond the API. It runs in-process and may install its own hooks. |
| Custom FFX upscaler | `custom/<name>/ffx` (DX12), `custom/<name>/ffx_vk`; `[Upscalers] Dx12Upscaler=<name>` | When the upscaler is first created | It **is** the FFX upscaler: `ffxCreateContext` (with the D3D12 backend desc, i.e. the game's device), `ffxConfigure`, `ffxQuery`, `ffxDispatch` with `ffxDispatchDescUpscale` = command list, color, depth, motion vectors, exposure, reactive / T&C masks, output, jitter, MV scale, render / upscale size, pre-exposure, camera near / far / FoV, reset (`FFXFeature_Dx12::EvaluateInternal`). DX12 and Vulkan only; the D3D11 provider does not use custom dlls. |
| Custom NGX runtime | `custom/<name>/dlss` | When DLSS is created | An `nvngx.dll` replacement: NGX init with the device, create / evaluate with the NGX parameter block. It then has to do the upscale itself. |
| Custom DLSSG | `custom/<name>/dlssg`; `[FrameGen] FGNvngxReplacement=<name>` with `FGInput=nvngxfg` or `FGOutput=dlssg` | When Nvngx FG is created | An `nvngx_dlssg` replacement: `DLSSG.Backbuffer`, `DLSSG.HUDLess`, `DLSSG.Depth`, `DLSSG.MVecs`, camera constants, and it writes `DLSSG.OutputInterpolated`. Streamline's `sl.dlss_g` (or OptiScaler's `DLSSG_Dx12` output, which drives it) owns the swapchain and the present. |

### 1.3 What OptiScaler exposes without the API

OptiScaler exports the NGX entry points by name (`NVSDK_NGX_D3D12_EvaluateFeature` etc.). Its own DX12 input paths, `FSR2_Dx12`, `FSR3_Dx12`, `FfxApi_Dx12`, `FfxApiExe_Dx12`, `XeSS_Dx12`, all call `NVSDK_NGX_D3D12_EvaluateFeature`, and the DLSS path is that export. So one detour of that export sees **every DX12 upscaler evaluate**, whatever the game's input and whatever the selected output upscaler, with the command list and the full NGX parameter block (Color, Output, Depth, MotionVectors, ExposureTexture, jitter, MV scale, reset, render subrect). That is a public, versioned ABI (NGX), not an OptiScaler internal. The PoC (5) uses exactly this.

## 2. Per need

Legend: **today** = works with stock OptiScaler now (API or self-hook on a public ABI); **PR** = needs a small upstream change (scope in 3); **no** = not achievable as a plugin without a large upstream change.

### 2.1 NR stage (single GPU)

| Need | Status | How / what is missing |
|---|---|---|
| Load into stock OptiScaler | today | ASI in `plugins/` + `LoadAsiPlugins=true`. Not as a custom FFX upscaler: that ties NR to FSR and makes the plugin re-host the real FSR provider. |
| Upscaler-evaluate inputs: color, output, depth, MVs, exposure, jitter, MV scale, reset, sizes | today (self-hook) / PR for a clean API | Detour of `NVSDK_NGX_D3D12_EvaluateFeature` (PoC). Covers all DX12 inputs. Not covered: the D3D11 and Vulkan paths (`Dx11wDx12`, `VkwDx12` bridges: their own exports `NVSDK_NGX_D3D11_*` / `NVSDK_NGX_VULKAN_*`, could be detoured the same way, but the D3D12 resources live inside OptiScaler's bridge). PR 1 makes it an API. |
| Pre placement (replace Color before the upscaler) | today (self-hook) | In the detour: record NR into the game's list, `params->Set(NVSDK_NGX_Parameter_Color, nrOut)`, call the original, restore. |
| Post placement (NR on Output) | today (self-hook) | Record after the original returns, into the same list. |
| Restore the game's compute / graphics root signature after our passes | PR | OptiScaler restores only around its own evaluate (`D3D12Hooks::RestoreRoot`, `RestoreComputeSignature`). Its tracker sees the plugin's `SetComputeRootSignature` calls as the game's, so after a pre pass it would "restore" the plugin's signature. PR 1 exposes the save / restore. A plugin-side tracker (own detours on `SetComputeRootSignature` / `SetGraphicsRootSignature` / `SetDescriptorHeaps`) works today but duplicates OptiScaler's. |
| Command queue: the lmxxf host enqueues HIP after the game's list is submitted (`EnqueueHipAsync` from the `Submitted` hook, `ExecuteCommandLists` split) | today (self-hook) | As AMDNR does it: a probe queue on the game's device, detour of `ID3D12CommandQueue::ExecuteCommandLists` (vtable slot 10). Self-contained; chains with OptiScaler's own queue hooks. |
| Terminal placement (NR on the HUD-less image at present) | PR | The HUD-less copy is HUDfix's (`Hudfix_Dx12`, `IFGFeature_Dx12::SetResource(HudlessColor)`), not reachable by a plugin; present-time callback missing too. PR 2. |
| Neural menu (placement, look, network, strength, tier, stats) | today, with limits | `RegisterTab` + Section / Combo / SliderFloat / SliderInt / Checkbox / Text / TextColored cover the controls. Missing: plots (frame-time / hip_ms graphs), tables, progress bars, images (debug views). A rewrite, not a port of AMDNR's ImGui code. PR 4 (optional). |
| Overlay stat (hip_ms, tier, placement on the FPS overlay) | PR | No overlay hook. PR 3. Today the stat can only live in the tab. |
| Config | today | `[Plugin.<name>]` via `Get*` / `Set*`; keys are the plugin's own (no `[DlssNr]`). |
| Log | today | `api->Log`. |
| Ray Regeneration interplay (NR after an RR evaluate) | today (self-hook) | RR also arrives through the NGX evaluate (the DLSS-D feature); the detour can tell it apart by the feature it was created for (hook `NVSDK_NGX_D3D12_CreateFeature` too). |

**Verdict (b): viable today as an ASI plugin**, for DX12 games, with pre and post placement, its own menu tab and settings. The pieces that rely on self-hooks are on public ABIs (NGX exports, D3D12 vtables), not on OptiScaler internals. Terminal placement, the overlay stat and a shared root-signature restore need small upstream PRs.

### 2.2 Multi-GPU terminal stage

| Need | Status | Why |
|---|---|---|
| Device / adapter redirect (game device on the render card) | PR | A plugin can detour `D3D12CreateDevice` itself, but OptiScaler's `hkD3D12CreateDevice` decides which device is the game's (`State::currentD3D12Device`, skipped when `nonPrimaryGpu`), and it treats the card without monitors as non-primary: with the game on that card OptiScaler would capture the plugin's display-card device instead. Its adapter order and spoofing (`PreferDedicatedGpu`, `IdentifyGpu`) also sit in the factory hooks. PR 5. |
| The plugin's own device on the display card | PR | Its `D3D12CreateDevice` and its queues go through OptiScaler's hooks (device capture, queue / `ExecuteCommandLists` hooks, spoofing). Needs an "unhooked" create or an exclusion list. PR 5. |
| Output passthrough (the render card has no outputs: `EnumOutputs`, `GetContainingOutput`) | today (self-hook) | OptiScaler does not hook `EnumOutputs`; the plugin can. |
| Swapchain creation redirect (real flip-model swapchain on the display card's queue, the game gets a proxy) | PR (medium) / no without it | OptiScaler wraps the swapchain in `FGHooks::CreateSwapChain` / `WrappedIDXGISwapChain4` (FG proxy, menu overlay, HUDfix present hooks). A plugin hook placed outside OptiScaler's would hand OptiScaler the display card's swapchain while the game renders on the render card, which breaks FG and HUDfix; it cannot place itself inside OptiScaler's hook. PR 6: a swapchain provider callback at the point OptiScaler calls the real factory. |
| Present interception (after the menu / overlay are drawn, before the real present) | PR | PR 2. |
| FG inputs at frame end (HUD-less, back buffer with UI, depth, MVs, camera) | PR | HUD-less, depth and MV copies are OptiScaler's FG / HUDfix internals. The custom DLSSG route receives them but cannot own the present, and exists only where DLSSG runs. PR 2. |
| Cross-adapter ring, display presenter, NR on the display card, UI composite | today | Plugin-internal once it has the resources and its own device (`SHARED_CROSS_ADAPTER` heap, fences, own thread). |
| OptiScaler FG with multi-GPU | no (for now) | FG presents through its own swapchain on the game's device. The design turns FG output off (MGPU_DESIGN 7.9); interpolation on the display card would be the plugin's own later item. |
| Menu section, overlay stats, `[Mgpu]` settings | today / PR 3 | As in 2.1. |

**Verdict (a): not viable on stock OptiScaler today.** The blockers are the device capture logic, the swapchain wrapping and the frame-end resources, all inside OptiScaler. PRs 2, 5 and 6 would make it viable; PR 6 is the only one that is more than a callback.

## 3. Upstream PRs that close the gaps

All as appended `OptiPluginApi` fields (version stays compatible through `OPTI_API_HAS`). Callbacks get a struct with a `size` field so it can grow.

**PR 1: upscale callbacks + root-signature restore (small).**

```c
typedef struct OptiUpscaleFrame {
    uint32_t size; uint32_t api;             // OptiApi_D3D12 (D3D11 / Vulkan later)
    void* commandList; void* device;
    void* color; void* output; void* depth; void* motionVectors; void* exposure;
    uint32_t renderWidth, renderHeight, outputWidth, outputHeight;
    float jitterX, jitterY, mvScaleX, mvScaleY, preExposure;
    bool reset; bool rayReconstruction; uint64_t frameId;
    void* colorOverride;                      // pre only: plugin output used as the upscaler's color
} OptiUpscaleFrame;
typedef void (*OptiUpscaleFn)(const OptiUpscaleFrame* frame, void* userData);
bool (*RegisterUpscaleCallbacks)(OptiPlugin*, OptiUpscaleFn before, OptiUpscaleFn after, void* userData);
void (*RestoreRootSignatures)(void* commandList);   // D3D12Hooks::RestoreRoot
```

Invoked in `inputs/NVNGX_DLSS_Dx12.cpp` `TryEvaluateOptiFeature` around `feature->Evaluate` (the same two sites the fork uses for AMDNR's `EvaluateBeforeUpscale` / `EvaluateAfterUpscale`), and in `IFeature_Dx11wDx12::Evaluate` / `IFeature_VkwDx12::Evaluate` with the D3D12 copies. Gives D3D11 and Vulkan-bridge games NR too, which a self-hook cannot.

**PR 2: frame-end callback with the FG resource set (small to medium).**

```c
typedef struct OptiPresentFrame {
    uint32_t size; void* swapchain; void* queue; void* backbuffer;
    void* hudless; void* depth; void* motionVectors;  // NULL when not captured this frame
    float cameraNear, cameraFar, fovY, jitterX, jitterY, mvScaleX, mvScaleY; bool reset; uint64_t frameId;
    uint32_t syncInterval, flags; bool* skipPresent;
} OptiPresentFrame;
bool (*RegisterPresentCallback)(OptiPlugin*, void (*fn)(const OptiPresentFrame*, void*), void* userData);
```

Invoked in `FGHooks::FGPresent` after `fg->Present()` / `Hudfix_Dx12::PresentStart()`, and in `WrappedIDXGISwapChain4::Present` (`LocalPresent`) when FG is off, after the menu overlay. Needs OptiScaler to keep collecting FG inputs (HUDfix + upscaler inputs) when a plugin asks for them with FG output off: a `bool (*RequestFrameResources)(OptiPlugin*, uint32_t mask)`.

**PR 3: overlay lines (small).**

```c
typedef size_t (*OptiOverlayTextFn)(char* buffer, size_t bufferSize, void* userData);
bool (*RegisterOverlayText)(OptiPlugin*, const char* label, OptiOverlayTextFn fn, void* userData);
```

Called from the FPS overlay draw in `menu/menu_common.cpp`, one row per plugin, honouring the overlay presets (shown in the fuller presets only).

**PR 4: more widgets (small, optional).** `PlotLines(label, const float* values, int count, float min, float max)`, `ProgressBar(fraction, overlay)`, `BeginTable / TableNextColumn / EndTable`. Covers the Neural tab's graphs and the MGPU link / ring table.

**PR 5: device and adapter hooks (medium).**

```c
bool (*RegisterAdapterOverride)(OptiPlugin*, void* (*pick)(void* requestedAdapter, void* userData), void* userData);
HRESULT (*CreateD3D12DeviceUnhooked)(void* adapter, int minFeatureLevel, const void* riid, void** device);
void (*ExcludeDevice)(void* device);   // OptiScaler's queue / list / resource hooks ignore it
```

`RegisterAdapterOverride` runs in `hkD3D12CreateDevice` before `o_D3D12CreateDevice`, and the picked adapter is the one OptiScaler treats as the game's (the `nonPrimaryGpu` check uses it). The factory's `EnumAdapters*` order follows it.

**PR 6: swapchain provider (medium, the one design discussion).**

```c
// Return S_OK with *swapchain set to take over creation; OptiScaler then wraps that object as usual.
bool (*RegisterSwapchainProvider)(OptiPlugin*, HRESULT (*create)(void* factory, void* gameQueue, void* hwnd,
    const void* desc1, const void* fullscreenDesc, void** swapchain, void* userData), void* userData);
```

Called in `FGHooks::CreateSwapChain` / the `DxgiFactory_Hooks` create paths at the point OptiScaler calls the real factory. The plugin returns its proxy (render-card buffers, real swapchain on the display card), and OptiScaler's FG proxy, menu and HUDfix keep wrapping what the game sees.

PRs 1 and 3 are enough to ship the single-GPU plugin cleanly; 2 adds terminal placement; 2 + 5 + 6 add multi-GPU.

## 4. Licence

- Upstream OptiScaler is GPL-3.0. `optiscaler_plugin.h` carries no separate licence, so it is GPL-3.0 as part of that repo. Whether a plugin that only includes it (declarations of a C function table) and loads in-process is a derivative work is unsettled; the conservative reading treats it as one. A one-line upstream PR adding an explicit permissive licence (or a GPL linking exception) to `plugin_sdk/` would settle it; until then the plugin should be GPL-3.0-compatible.
- **Reusing AMDNR code** (its host passes, look rules, Neural menu): AMDNR is GPL-3.0 with additional terms under section 7(b), (c), (e) (`Licenses/AMDNR_NOTICE.txt`): keep its notice and copyright lines, credit "AMDNR by 3zwr1" in the README and wherever credits are shown, mark modified menu code as changed, no use of the AMDNR name or logo. The plugin is then GPL-3.0 with those terms, and its source must be published with each release. That is compatible with the target (public repo), but the product would carry AMDNR's terms.
- **Writing it fresh against lmxxf's MIT API** (from lmxxf's own repository, not AMDNR's copy, which has "Fork addition" fields such as `LMXXF_NR_FRAME_FLAG_TEMPORAL` / `CONTROLS` that are AMDNR's): the plugin's own code can be MIT, or GPL-3.0 for safety with respect to the plugin header (above). No AMDNR terms apply to code that does not contain AMDNR material.
- **The runtime binary**: lmxxf's source is MIT, but the `LmxxfNrRuntime.dll` / `.pak` that users have come from AMDNR's release and contain AMDNR's work (pak format, c32w kernels, host rules). Loading a user-installed copy at run time does not change the plugin's licence, but the plugin must not redistribute it, and features that depend on AMDNR's runtime additions (the controls block, temporal history) tie the plugin to that build. A runtime built from lmxxf's MIT source needs the weight extraction from a locally owned `nvngx_dlssnr.dll` (MGPU_DESIGN 2.3).

## 5. Proof of concept (`mgpu/plugin_poc/`)

`NrPlugin.asi`: an ASI plugin for stock OptiScaler (plugin API v1).

- `OptiPluginInit` logs the API version, reads `[Plugin.NrPlugin] Placement / LogEvery / LogInputs`, finds OptiScaler's module from the API table's address (works whatever the DLL is named), and detours its `NVSDK_NGX_D3D12_EvaluateFeature` export with Detours (the copy vendored under `src/OptiScaler`).
- The detour reads Color, Output, Depth, MotionVectors, ExposureTexture (size, format), jitter, MV scale and reset from the NGX parameter block, times the upscaler call, and logs the first evaluate and every `LogEvery`-th.
- A "NR Plugin" tab with a placement combo (recorded only), log settings, a Save button and live stats (evaluate count, upscaler CPU time, the last frame's inputs).
- No NR runs and nothing is written to the game's command list. The overlay stat is not possible on API v1 (PR 3); the stats are in the tab.

Build: `mgpu/plugin_poc/build.ps1` (VS 2022, v143, Release x64; parallelism = logical threads - 2). Output: `x64/plugin_poc/NrPlugin.asi`; it imports only `KERNEL32.dll`.

Install: `<MainDllPath>/plugins/NrPlugin.asi`, and in `OptiScaler.ini` (below). `MainDllPath` is the `OptiScaler\` libraries folder next to the game's exe when it exists (`[Libraries] OptiDllPath`, default `OptiScaler`), otherwise the exe folder. With the usual install that is `<game exe folder>/OptiScaler/plugins/`.

```ini
[Plugins]
LoadAsiPlugins=true

[Plugin.NrPlugin]
LogEvery=300
```

Log lines (OptiScaler log):

- `Loaded: ...\plugins\NrPlugin.asi`, `Initializing plugin API for NrPlugin`
- `[NrPlugin] NrPlugin PoC loaded, plugin API v1 (size N), placement=after upscaling`
- `[NrPlugin] hooked NVSDK_NGX_D3D12_EvaluateFeature (DX12 upscaler evaluates)`
- `[NrPlugin] Registered tab: NR Plugin`
- once the game upscales: `[NrPlugin] evaluate #1 color=WxH fmtF output=... depth=... mv=... exposure=... jitter=... mvscale=... reset=... result=0x1 cpu=...ms`

Next step for the PoC, if the direction is taken: load `LmxxfNrRuntime.dll` through `LmxxfNrGetApi(1, ...)`, `Create` on the game's device and queue, and run the post placement inside the detour (`PrepareFrame`, `RecordInputs`, `EnqueueHipAsync` from an `ExecuteCommandLists` detour, `RecordOutputs`), with a plugin-side root-signature restore until PR 1 exists.

## 6. Recommendation

1. **Build the NR stage as an ASI plugin on stock OptiScaler**, written fresh against lmxxf's MIT API, using AMDNR's published code as reference only. It works today for DX12 games (pre / post placement, its own tab and settings), and it stops depending on a fork that has to re-apply 98 modified upstream files on every OptiScaler update.
2. **Open PR 1 and PR 3 upstream** (upscale callbacks with root-signature restore, overlay lines). They are small, generic (any post-process plugin wants them) and replace the plugin's self-hook with an API that also covers D3D11 and Vulkan games. Then PR 2 for terminal placement.
3. **Keep multi-GPU in the fork** (`mgpu/terminal`) until PRs 5 and 6 are discussed upstream. Its plugin-internal parts (ring, presenter, display-card NR, UI composite) should be written so they move into the plugin unchanged once those hooks exist.
