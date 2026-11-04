# Multi-GPU Neural Rendering: design

Status: design, nothing implemented yet. The branch is `mgpu/onboarding`.

This fork is a modified AMDNR. It is not the original AMDNR and is not endorsed by its author. The credit for AMDNR goes to 3zwr1 (https://github.com/3zwr1/AMD-NR---OptiScaler), and AMDNR's notice is kept in `Licenses/AMDNR_NOTICE.txt`.

## 1. Goal and topology

- **The game renders on the second card**, the one with no monitors (the "render card").
- **At the end of each frame**, the frame-generation inputs are sent one way over PCIe to the card the monitors are on (the "display card"). Those inputs are:
  - the HUD-less colour;
  - the back buffer with the UI;
  - depth, motion vectors and camera data.
- **The display card** then:
  - runs DLSS 5 Neural Rendering through the lmxxf runtime on the HUD-less image;
  - composites the UI back on top;
  - presents (flip model, `ALLOW_TEARING`, `Present(0)`), so the swapchain keeps Hardware: Independent Flip and VRR.
- **The NR stage runs in parallel with the next frame's rendering.** Offloading mid-frame, with the render card waiting for the answer, was measured as a net loss (about 22 ms at Gen4 against 13 ms inline), and that rules it out.

```
render card:  [render N] [post+UI N] -> copy N into ring slot  |  [render N+1] ...
                                           | PCIe (system memory, one way)
display card:                          [ingest N] [lmxxf NR N] [UI composite N] [Present N]
```

### Reference configuration

- Two RDNA 4 cards (gfx1201).
- The display card's host link is PCIe 5.0 x8, measured at about 28 GB/s.
- The render card's host link is PCIe 4.0 x8, about 14 GB/s.
- There is no GPU peer-to-peer on Windows. A `SHARED_CROSS_ADAPTER` heap lives in system memory, and each side moves data at its own host-link speed.
- `CrossAdapterRowMajorTextureSupported` = 0, so the ring uses buffers with placed footprints, not shared textures.

## 2. What AMDNR does today

Source references are relative to `main/AMDNR-source/`, the 0.3.4.2 source snapshot. Where a file is missing from that snapshot, the reference is marked *(upstream)* and points to current upstream OptiScaler.

### 2.1 Where the NR stage runs

- **Entry points.**
  - `DlssNr::AmdBridge::Before` runs before Super Resolution (`optiscaler/dlssnr/amd/AmdBridge.cpp:2085`).
  - `After` runs after an upscale or Ray Reconstruction evaluate (`:2089`).
  - Both call `Run(cmd, params, queue, post, epoch)` (`:1510`).
- **Pre-SR.** The pass reads `NVSDK_NGX_Parameter_Color` at render size and swaps the NR result into the parameter block (`:2065-2071`). `Restore` puts the original back.
- **Post (the `After` placement).** The pass reads `Output` at display size (`:1820-1833`) and writes back into it (`AmdPreSr.h:62-69`).
- **Placement in 0.3.5.x.** `[DlssNr] AmdPlacement=post` is the default under the Full Model Look. The CONTROL Resonant baseline below ran it: the network was fed a 1080 tier and its edit lifted to the 3840x2160 upscaler output.
- **Call sites.**
  - The native DX12 sites are in `inputs/NVNGX_DLSS_Dx12.cpp`, which is missing from the snapshot (two sites, `optiscaler/dlssnr/README.md:28`).
  - The D3D11 and Vulkan bridges show the same pattern: `EvaluateBeforeUpscale`, then `Evaluate`, then `Restore`, then `EvaluateAfterUpscale` (`optiscaler/upscalers/IFeature_Dx11wDx12.cpp:462-483`, `IFeature_VkwDx12.cpp:2366-2385`).
- **Recording.** Everything is recorded into the game's own command list.
  - The bridge detours `ID3D12CommandQueue::ExecuteCommandLists` (vtable slot 10) on a probe queue it creates on the game's device (`AmdBridge.cpp:1685-1704`).
  - Each batch is split around the NR list as `Submitting`, then the real `ExecuteCommandLists`, then `Submitted` (`:302-351`).
- **Interleave** (the model runs every Nth frame and the others are reprojected) is part of the backend (`Interleave.h:12-16`, `LmxxfBackend.cpp:3094`).
- **Relation to frame generation and HUDfix.**
  - NR attaches only to upscaler evaluates, never to FG (`DlssNrFeature_Dx12.h:14-28`).
  - So FG and HUDfix see the colour NR has already modified.
- **Final image mode.** `dlssnr/amd/PresentExperimental.h` holds a present-time, whole-frame mode on the back buffer (`Render` at `:491`). It is compiled out (`kAvailable = false`, `:104`).
  - It is called from `menu/menu_overlay_dx.cpp:468` before the menu is drawn.
  - It refuses to run while a bridge backend exists (`:503-508`), because HIP and the runtime are process-global.
  - This is the closest existing hook to the terminal stage.

### 2.2 The lmxxf runtime host

- **API.** `optiscaler/dlssnr/lmxxf/runtime/LmxxfNrApi.h` is a C ABI (v1).
  - The DLL is `LmxxfNrRuntime.dll`: MinGW-built, lmxxf by Kien (MIT), plus AMDNR's kernels.
  - The pak is `LmxxfNrRuntime.pak`: one encrypted and authenticated file holding the weights, HIP modules and HLSL, read in memory.
  - Both are found next to `OptiScaler.dll`, or in `DLSS5-AMD\` (`LmxxfBackend.cpp:41, :944-963`). The runtime checks the pak; the host does not hash the DLL (`:1828`).
- **Session.** `Create` receives the game's `ID3D12Device` and `ID3D12CommandQueue` (`:1864-1876`).
  - The runtime matches the HIP device to the D3D12 device by LUID (baseline log: `hip_device=0 (luid)`).
  - It imports its D3D12 buffers and fence into HIP itself. The host sees only `OutputReady`.
  - **For MGPU:** giving `Create` the display card's device and queue places the network on the display card, with no change to the runtime.
- **Per frame.**
  - `Record` first consumes frame N-1: `OutputReady`, then `RecordOutputs`, then `Retire` (`:2692-2742`).
  - It copies the colour to FP16 (`:2919-2953`), then calls `PrepareFrame` (`:3254`) and `RecordInputs` (`:3365`).
  - `EnqueueHipAsync` is called from the `Submitted` hook, after the game's list has been executed (`:4139`).
  - The edit lands one frame late, carried by the motion vectors.
- **Sizes.** `model_scale` 0.25-1. Network tiers are 360 / 576 / 720 / 900 / 1080 (`LmxxfTierPolicy.h:7-36`), plus a 2560x1440 size under the NVIDIA look.
  - With the post placement, the host feeds the network a downsampled picture and lifts the edit back to output size with a guided upsample (`AmdDetailLift`).
- **Getting the files.**
  - Since 0.3.5.8 the lmxxf pair is a separate download: `AMDNR-lmxxf-0.3.5.8R2.zip` on release Alpha0.3.5.8R2, checked against that release's `SHA256SUMS.txt` and the GitHub asset digest.
  - It is signed by a self-signed "AMDNR (3zwr1)" certificate, so Windows reports the signature as untrusted.
  - The release zip `AMDNR-v0.3.5.8R13.zip` no longer contains it.

### 2.3 Weights

- **AMDNR** does not generate weights from the user's `nvngx_dlssnr.dll`. They come inside the encrypted pak.
- **lmxxf's own repository** extracts them from a locally owned `nvngx_dlssnr.dll` with the `Development/prepare_native_*_gpu.py` scripts. Its README calls these "not a polished pipeline". The loose files (`native-game-tiled-assets`, about 0.6 GB on the GPU) are what a runtime built from lmxxf's source needs.
- **MGPU uses the pak as installed.** The user's NVIDIA DLLs are not needed on this path. Building the runtime from lmxxf's MIT source would need the extraction step. That is a separate decision, because those scripts depend on reverse-engineered layouts of NVIDIA's DLL.

### 2.4 The danielblnc runtime and NeuralHub

- **How AMDNR drives his DLLs.** AMDNR loads `dlssnr_amd_pass1..3.dll` and `dlssnr_on_amd_weights.bin` (`AmdPreSr.cpp:1967-2008`). It then drives them through per-build layout rows of RVAs into his binary (`AmdLayout.h:56-59, :201+`):
  - it calls functions at fixed offsets;
  - it writes his globals (`AmdPreSr.cpp:2054-2066`);
  - it patches an IAT slot for `D3DCompile` (`:2039-2050`);
  - it blocks his bootstrap thread through a `CreateThread` detour (`RuntimeHostLoad.h:163-261`).
- **MGPU does not use or extend this path.** It patches a binary that may not be modified, so the multi-GPU code is lmxxf only.
- **NeuralHub 0.8.0.** AMDNR identifies it by hash and does not drive it ("NotRegistered+VersionNotRead", baseline log). AMDNR also warns when danielblnc's standalone loader sits next to it (`AmdBridge.cpp:170-262`). The two must not be installed in the same game folder.

### 2.5 FSR Ray Regeneration and XeSS-FG

- **FSR Ray Regeneration** is `Upscaler::FSR_RR`. It is built on `ffxDispatchDescDenoiser` (`optiscaler/upscalers/fsr31/FSRDFeature_Dx12.cpp:1-53`) and gated per card by `hooks/RrHardwareGate.h:8-12`.
  - NR runs `After` on the output.
  - A failed RR moves NR back to pre-SR (`FSRDFeature_Dx12.cpp:1394-1396`).
  - RR stays on the render card in MGPU, because it runs mid-frame.
- **XeSS-FG** is an `IFGFeature_Dx12` (`framegen/xefg/XeFG_Dx12.cpp:394-416, :957, :1530-1647`). It is not tied to the NR bridge.

### 2.6 Frame-generation inputs, HUD-less colour, present

- **Resource types:** Depth, Velocity, HudlessColor, UIColor, Distortion (`framegen/IFGFeature.h:25-34`).
  - Depth and MVs come from the upscaler's parameters (*(upstream)* `inputs/FG/Upscaler_Inputs_Dx12.cpp:100-180`).
  - FFX FG descriptors are another source (*(upstream)* `inputs/FG/FfxApi_Dx12_FG.cpp`).
- **HUDfix**
  - It watches render-target binds, draws and dispatches (`resource_tracking/ResTrack_dx12.cpp:1075-1732`).
  - It takes the first resource at swapchain size in the swapchain's format group written after the upscale (`hudfix/Hudfix_Dx12.cpp:336-544`).
  - It copies that resource and passes it to `SetResource(HudlessColor)` (`:830-873`).
- **FSR-FG** reads depth, velocity and HUD-less (`framegen/ffx/FSRFG_Dx12.cpp:322-409`). It extracts the UI as a HUD copy plus a comparison against the HUD-less colour (`:614-760`).
- **Present chain**
  - `DxgiFactory_Hooks.cpp:245, :629` calls `FGHooks::CreateSwapChain` (`hooks/FG_Hooks.cpp:92-200`). That creates the proxy swapchain and wraps the real one as `WrappedIDXGISwapChain4` (`DxgiFactory_Hooks.cpp:604, :1030`).
  - At present: `FGHooks::FGPresent` (`FG_Hooks.cpp:1101`), then `fg->Present()` (`:1222`), then `Hudfix_Dx12::PresentStart()` (`:1232`), then `o_FGSCPresent` (`:1271`).
  - Then `WrappedIDXGISwapChain4::Present` calls `LocalPresent` (`wrapped/wrapped_swapchain.cpp:153`), then the menu overlay (`:370`).
- **Device and adapter hooks**
  - `hkD3D12CreateDevice` (`hooks/D3D12_Hooks.cpp:1338`; the original is called at `:1396-1407`).
  - `EnumAdapters`, `EnumAdapters1`, `EnumAdapterByLuid` and `EnumAdapterByGpuPreference` (`hooks/DxgiFactory_Hooks.cpp:1959-2074`), with the GPU order from `misc/IdentifyGpu.cpp:70-75`.

### 2.7 Config and menu

- **The `[DlssNr]` block** is `Config.h:282-1050`. It is read at `Config.cpp:331-664` and written at `Config.cpp:1507-1799`.
- **The Neural tab** is `DlssNr::RenderMenu` (`optiscaler/dlssnr/DlssNr_Menu.cpp:1210`), called from `menu/menu_common.cpp:8312-8326`. Its sections are declared in `optiscaler/dlssnr/menu/NeuralUi.h:308-331`.

### 2.8 Build status of this snapshot (blocker)

- **The tree is incomplete.**
  - 244 of the 545 project items are missing: `inputs/`, `proxies/`, `shaders/`, `low_latency/` and `external/`.
  - Some of the missing files exist only in AMDNR and are not public: `shaders/dlssnr/*`, `shaders/fsrd_preprocess/*`, `proxies/XeFGPacing.h`, and AMDNR's NGX `nvsdk_ngx_defs_dlssd.h`.
- **The snapshot is 0.3.4.2.** The current binaries are 0.3.5.8R13. AMDNR's README says the source will be published with 0.5.0.
- **What a reconstruction got to.** The test overlaid the snapshot on upstream OptiScaler `c983a500` (2026-09-01, the closest blob match) with its submodules.
  - The tree compiles except for about 16 translation units.
  - The causes are the missing AMDNR-only headers, D3D12 declarations newer than Windows SDK 10.0.22621 (`D3D12_STATIC_SAMPLER_DESC1`, root signature 1.2), and the project's `v145` toolset.
  - With the VS 2022 compiler (`v143`), raw string literals over 16 KB also need rewriting.
- **Consequence:** this fork cannot produce a working `OptiScaler.dll` yet. Milestone M2 below decides the base.

## 3. Baseline (single card, inline)

AMDNR 0.3.5.8R13 with the lmxxf pair from 0.3.5.8R2, in CONTROL Resonant, on the display card. One short launch with no input, reaching the intro screens only:

- **Placement:** post. The 3840x2160 output was fed to the network at 1920x1080. Guides (depth, MV) were at 1920x1080. Full network, 71 blocks.
- **Network time:** `hip_ms` mean 11.05 ms (max 11.5) over 600-frame windows.
- **Memory:** about 5.5 GB VRAM in use.
- **Not measured yet:** the fed image was black (no gameplay frame was reached), so this run gives no image or fps comparison. A gameplay baseline (NR off / inline) needs a person to load a save.

## 4. Data per frame and transfer time

4K output, render resolution 1920x1080 (CONTROL's FSR-FG input set):

| Item | Format | Size |
|---|---|---|
| HUD-less colour | 3840x2160 R10G10B10A2 | 33.2 MB |
| Back buffer (with UI) | 3840x2160 R10G10B10A2 | 33.2 MB |
| Depth | 1920x1080 R32 | 8.3 MB |
| Motion vectors | 1920x1080 RG16F | 8.3 MB |
| Seal + camera constants (jitter, near/far, FoV, MV scale, reset, frame id) | | < 1 KB |
| **Total (dense)** | | **82.9 MB** |

| | Render card writes (Gen4 x8, 14 GB/s) | Display card reads (Gen5 x8, 28 GB/s) | Serial | Pipelined (per frame) |
|---|---|---|---|---|
| Dense, 82.9 MB | 5.9 ms | 3.0 ms | 8.9 ms | 5.9 ms |
| UI as 64x64 tiles, about 10% covered (53 MB) | 3.8 ms | 1.9 ms | 5.7 ms | 3.8 ms |
| Without depth (lmxxf needs MVs only; AMDNR's guards use depth) | 5.3 ms | 2.7 ms | 8.0 ms | 5.3 ms |

What the numbers mean:

- **Budget.**
  - At 60 fps (16.7 ms) the dense package uses 35% of the frame on the copy engine.
  - At 120 fps (8.3 ms) it uses 71%.
  - The copies run on copy queues and overlap rendering, but they share the render card's host link with the game's own uploads.
  - The 14 / 28 GB/s figures are idle-link numbers. Earlier measurements with both cards under load were 2-3x slower, so M0 re-measures under load.
- **Display card per frame:** NR about 11 ms, plus about 1 ms of host passes (downsample, detail lift, UI composite), plus the ingest copy on its copy queue. That caps NR at roughly 80-85 fps. Above that, the presenter skips to the newest slot, or interleave runs the model every second frame.
- **Latency:** about one extra frame (the ingest copy plus NR), in exchange for removing about 11 ms of network time from the render card.

## 5. Change list

### 5.1 New modules (`optiscaler/mgpu/`)

| File | Role |
|---|---|
| `MgpuConfig.h/.cpp` | The `[Mgpu]` settings block (5.3) and the topology chosen at start (which card renders and which presents, by LUID, PCI location or index). |
| `DeviceRedirect.h/.cpp` | Forces the game's device onto the render card. In `hkD3D12CreateDevice`, the `pAdapter` passed in is replaced with the render card's adapter before `o_D3D12CreateDevice` (`hooks/D3D12_Hooks.cpp:1396-1407`). This also covers `sl.interposer`, whose call reaches d3d12's export. The `EnumAdapters*` order is aligned so the game sees one consistent adapter (`DxgiFactory_Hooks.cpp:1959-2074`). The display card's device is created here too, early, with its own direct and copy queues. |
| `OutputPassthrough.h/.cpp` | The render card has no outputs. This module wraps that adapter's `EnumOutputs` and the swapchain's `GetContainingOutput`, and returns the display card's real `IDXGIOutput6`. That covers `GetDesc1` for HDR, `GetDisplayModeList1`, `WaitForVBlank` and `FindClosestMatchingMode`, and stops CONTROL from falling back to a 128x128 window. |
| `TerminalCapture.h/.cpp` | Runs on the render card at present time, in `FGHooks::FGPresent` after `fg->Present()` and `Hudfix_Dx12::PresentStart()` (`FG_Hooks.cpp:1222-1271`), or in `LocalPresent` when FG is off. It takes the frame's HUD-less colour, back buffer, depth, MVs and camera constants from the FG feature's resource copies (`IFGFeature_Dx12::GetResourceCopy`), records the copies into a ring slot on a render-card copy queue, and signals. The game's FG interpolation is turned off. FG input capture stays on. |
| `XAdapterRing.h/.cpp` | A one-way, triple-buffered ring in a `D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER` buffer heap, with placed footprints for each texture. Each slot has a 192-byte seal (magic, frame id, generation, sizes and formats, camera block, reset flag, CRC of the constants), after Neural-coprocessor's MIT design. Synchronisation: a shared "filled" fence (render card signals, display card waits); a shared "released" fence (display card signals after ingest, render card waits before it reuses a slot; values only, no data); skip-to-newest on the consumer. |
| `DisplayPresenter.h/.cpp` | Runs on the display card, on its own thread. Per slot: ingest copy (copy queue), then the lmxxf NR job, the edit lift and the UI composite (direct queue), then `Present(0, ALLOW_TEARING)` on the real flip-model swapchain. Also owns frame pacing, the `GetFrameStatistics` passthrough and the waitable object. |
| `SwapchainRedirect.h/.cpp` | Hooks swapchain creation. The real swapchain is created with the display card's queue on the game's HWND. The game gets a proxy whose `GetBuffer` returns render-card textures from a ring that matches the game's buffer count and format. `ResizeBuffers`, `SetFullscreenState`, `ResizeTarget`, `SetColorSpace1`, `SetHDRMetaData`, `CheckColorSpaceSupport` and `SetMaximumFrameLatency` are forwarded to the real swapchain. Builds on the existing FG proxy (`FG_Hooks.cpp:92-200`) and `WrappedIDXGISwapChain4`. |
| `UiComposite.h/.cpp` + HLSL | `mask = smoothstep(t0, t1, max3(abs(bb - hudless)))`, then `out = lerp(nr, bb, mask)`, the same idea as FFX and OptiScaler's HUD compare. Includes the 15% sanity gate: if more than 15% of pixels differ, the frame is treated as UI-only and passed through. Shown in the debug view. |
| `RemoteNr.h/.cpp` | The lmxxf session on the display card. It reuses `Lmxxf::Backend` with the display device and queue (`Create` binds HIP by LUID) and drives its post placement on the ingested HUD-less texture. The per-frame order follows the runtime's documented fast path: `PrepareFrame`, `RecordInputs`, `EnqueueHipAsync`, `RecordOutputs` gated by `OutputReady`, `Retire`. |

### 5.2 Changes to existing files

- **`hooks/D3D12_Hooks.cpp`:** the redirect call in `hkD3D12CreateDevice`. The non-primary flag (`:1361-1377`) must treat the render card as the game's device.
- **`hooks/DxgiFactory_Hooks.cpp`:** the adapter order, and the swapchain creation redirect (`:245`, `:629`).
- **`hooks/FG_Hooks.cpp`:** the terminal-capture call, and suppressing FG output while MGPU is on.
- **`dlssnr/amd/AmdBridge.cpp`:**
  - with MGPU on, `Before`/`After` return "not ours", so no NR runs on the render card;
  - `BackendActive` stays the single-runtime guard, and that runtime is the display card's;
  - the probe queue and `ExecuteCommandLists` detour must not attach to the display card's queues.
- **`dlssnr/lmxxf/LmxxfBackend.cpp`:**
  - accept an explicit device and queue that are not the game's;
  - remove the assumption that the NR list is submitted by the game (the presenter submits it itself, so `Submitted` is called directly).
- **`Config.h/.cpp`:** the `[Mgpu]` block, after `Config.h:1050` and the `[DlssNr]` read and write runs.
- **`dlssnr/DlssNr_Menu.cpp`, `dlssnr/menu/NeuralUi.h`:** a `DrawMultiGpu(ctx)` section next to `DrawTools`. It shows the topology, link speeds, ring state, transfer and NR timings, the skip count and the UI mask debug view.
- **`OptiScaler.vcxproj(.filters)`:** the new files.

### 5.3 Settings (`[Mgpu]`)

| Key | Values | Default |
|---|---|---|
| `Enabled` | true / false | false |
| `RenderAdapter` | `auto` (the card with no outputs) / LUID / PCI `bus:dev.fn` / index | auto |
| `DisplayAdapter` | `auto` (the card that owns the game's monitor) / same forms | auto |
| `UiSource` | `diff` (back buffer minus HUD-less) / `none` (whole frame, with the HUD going through NR) | diff |
| `UiThreshold` | the smoothstep edges | as FFX |
| `RingSlots` | 2..4 | 3 |
| `Fallback` | `inline` (NR on the render card) / `off` | inline |

The settings follow the project rules: a setting does only what its row says, and the menu has on/off rows plus the debug views.

## 6. Display passthrough

- **HDR.**
  - The real swapchain gets the game's format: 10-bit for HDR10/PQ, or FP16 for scRGB. Colour space and HDR metadata are forwarded.
  - The HUD-less colour carries the same encoding. In CONTROL it is R10G10B10A2, which may be PQ or SDR depending on the game's HDR setting; M1 checks which.
  - lmxxf's codec must be told whether its input is display-referred. AMDNR already handles display-referred input: RenoDX composition is refused there, and Classic composition runs.
- **Modes.** `GetDisplayModeList(1)`, `FindClosestMatchingMode`, `SetFullscreenState` and `ResizeTarget` are forwarded, plus refresh rate and rotation (portrait monitors). With several monitors, the monitor's identity is kept by returning the display card's real outputs.
- **Timing.**
  - `WaitForVBlank`, `GetFrameStatistics` and `GetLastPresentCount` come from the real swapchain.
  - The waitable object is the real one, so the game paces against the display card.
  - Anti-Lag 2 and Reflex markers stay on the render card's timeline, offset by the pipeline depth.
- **Gamma:** `SetGammaControl` is forwarded.
- **Overlays and capture.** Steam, RTSS and ReLive hook the visible swapchain, which is the display card's, so they keep working. The OptiScaler menu draws on the display card after the UI composite.
- **VRR.** There is one visible window and one flip-model swapchain, with `ALLOW_TEARING` and `Present(0)`. PresentMon must report "Hardware: Independent Flip". The DComp overlay path goes through DWM and is a fallback only.

## 7. Risks

1. **No buildable source** (2.8). Every in-game milestone depends on M2.
2. **Bandwidth under load.** Both host links carry game traffic. If the ring copy slows down the way it did when both cards were loaded in the earlier measurements, 120 fps is out of reach. Mitigations, which come after the features per project rules: sparse UI tiles, and sending only the UI difference.
3. **The display card is NR-bound** at about 11-12 ms per frame. That caps NR at roughly 80-85 fps, and interleave or skip-to-newest is needed above it.
4. **Display-referred input.** In the baseline, NR ran on linear upscaler output. The terminal stage feeds post-tonemap colour with post-processing already applied (grain, chromatic aberration, depth of field). The look may differ from AMDNR's post placement, and an A/B against inline is required.
5. **UI extraction.** Translucent and animated HUD elements can leak into the mask or fall out of it. The 15% gate and the debug view are needed. NR never sees the HUD itself, because the back buffer is composited after NR.
6. **The game's adapter logic.** CONTROL creates its device through `sl.interposer` with an explicit adapter, and with no outputs it shrank to a 128x128 window. The redirect and the output passthrough have to land together.
7. **Two devices in one process.** The probe-queue and `ExecuteCommandLists` detours are process-wide vtable hooks, so the display card's queues must be excluded from them. HIP binds per device by LUID, so one runtime session stays on the display card.
8. **Licensing.**
  - The danielblnc path is never used (2.4).
  - The lmxxf pair and the pak are installed by the user from AMDNR's release and never committed.
  - NVIDIA DLLs are not needed on this path.
9. **Frame generation.** The game's FG output is suppressed (FG inputs only). Interpolation on the display card after NR is a later item.

## 8. Milestones (each ends with a go/no-go)

| # | Milestone | Go if |
|---|---|---|
| M0 | Re-measure the links under load with `mgpu/tools/optinr_diag`, after the link change: the dense 83 MB package, pipelined, both directions, with and without a background load on both cards. | Pipelined ≤ 8 ms per package under load (60 fps with margin). |
| M1 | **Offline harness**, `mgpu/harness/`: (a) capture one still and one moving 10-frame sequence of CONTROL's FSR-FG inputs with `sl_tap` (HUD-less, back buffer, depth, MVs, camera; a person loads a save); (b) a standalone exe that creates a device on the display card, loads `LmxxfNrRuntime.dll` + `.pak` from `data/`, runs the documented call order on the captured HUD-less frames (downsample to the 1080 tier, NR, edit lift), composites the UI from the back buffer, and shows and saves the result. | The output differs from the input and the 10-frame still sequence is stable, with no flicker beyond lmxxf's own. The moving sequence has no trails beyond what AMDNR shows inline. The NR time on the display card is about the inline 11 ms. The UI is pixel-identical to the back buffer outside the mask. |
| M2 | **Choose the build base:** (a) AMDNR's full source once it is published (README: with 0.5.0); (b) upstream OptiScaler plus the AMDNR modules from the snapshot plus `optiscaler/mgpu/`, with the gaps filled; or (c) a separate MGPU module next to AMDNR's release DLL, which needs a public hook point and conflicts with its present hooks. | One tree builds with VS 2022 or 2026, and its `OptiScaler.dll` reproduces 0.3.5.8R13's inline NR in CONTROL (log lines and timings). |
| M3 | Device redirect + output passthrough. The game renders on the render card and presents through Windows' hybrid passthrough for now. | GPU engine counters show the render card. The game keeps its 4K window and modes. It is stable for 10 minutes. |
| M4 | Terminal capture + ring + display presenter with **no NR**: the display card presents the received back buffer. | Pixel-identical to the back buffer over a 10-frame moving sequence. PresentMon shows Hardware: Independent Flip. fps within 5% of native at 60 fps. The added latency is measured. |
| M5 | lmxxf NR on the display card + UI composite. | The fps gain over the inline baseline is measured, and the image matches M1. Still and moving 10-frame gates pass. The HUD is untouched. |
| M6 | Display passthrough: HDR (PQ and scRGB), fullscreen / mode changes, resize, VRR, frame statistics, latency markers, Steam, RTSS and ReLive overlays. | Each item checked once in CONTROL. VRR is confirmed on the monitor's OSD. |
| M7 | Menu section + `[Mgpu]` settings + fallback (inline NR when the second card or the runtime is missing). Then the optimisation pass: sparse UI tiles, RG16F depth, and re-profiling. | Clean on/off toggles. The fallback is verified. The optimisation is kept only if measured. |

## 9. Tools

`mgpu/tools/` holds the MIT diagnostics carried over from the archived `optinr-amd` project: `optinr_diag`, `gpu_pref_probe`, `p2p_probe`, `sl_tap`, the header fetch scripts and `sl_tap_view.py`. They have their own CMake; see `mgpu/tools/README.md`.
