# Multi-GPU Neural Rendering: design

Status: M3-M5 implemented on `mgpu/terminal` (see 10), and as part of the lmxxf_nr plugin on `mgpu/plugin` (see 11). The design branch is `mgpu/onboarding`; the build base (M2) is `mgpu/base`.

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
- **Consequence:** the snapshot alone cannot produce a working `OptiScaler.dll`. Milestone M2 below decides the base.
- **M2 (option b), branch `mgpu/base`:** upstream OptiScaler `c983a500` under `src/` as a git subtree, AMDNR's published sources ported on top, the gaps stubbed. It builds with VS 2022 (v143) and SDK 22621. What is recovered and what is stubbed: `docs/AMDNR_GAPS.md`.

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

## 10. Status on `mgpu/terminal` (M3-M5)

Code: `src/OptiScaler/mgpu/` (`MgpuCore` topology and redirect, `MgpuSwapChain` proxy + ring + presenter, `MgpuGpuLoad` per-card engine use, `MgpuDump` frame dumps, `MgpuHost` the only entry point for OptiScaler code: config, logging, NR backend, menu tab, overlay lines). Hook points in existing files are marked `[mgpu]`: `Config.h/.cpp` (the `[Mgpu]` block), `hooks/D3D12_Hooks.cpp` (redirect in `hkD3D12CreateDevice`), `hooks/DxgiFactory_Hooks.cpp` (no FG swapchain, proxy in `CreateSwapChainForHwnd`), `dlssnr/amd/AmdBridge.cpp` (`RuntimeGate`: no NR on the render card), `menu/menu_common.*` (Multi-GPU tab, overlay lines), `OptiScaler.vcxproj`, `OptiScaler.ini`. With `Enabled=false` (default) none of them changes behaviour.

What differs from the change list in 5:

- **One proxy instead of TerminalCapture + SwapchainRedirect.** The game's swapchain is a proxy `IDXGISwapChain4` whose back buffers are render-card textures; OptiScaler's `WrappedIDXGISwapChain4` and its menu sit on top of it unchanged. At `Present` a prerecorded copy-queue list (per back buffer and slot) copies the back buffer into a ring slot (placed footprint in a `SHARED | SHARED_CROSS_ADAPTER` heap) and signals a shared "filled" fence. The game queue waits for a buffer's last send before reusing it. The display card's presenter thread takes the newest scheduled frame (skip to newest), waits on "filled" on the GPU, copies the slot into the real back buffer (prerecorded per slot and real buffer), signals "released", runs NR, and presents (`FLIP_DISCARD`, `ALLOW_TEARING`, `Present(0, ALLOW_TEARING)` when the game uses sync 0). A slot is rewritten only after "released" reached the last frame the display card took from it. The game's lead over the display card is capped at two frames; a `FRAME_LATENCY_WAITABLE_OBJECT` swapchain gets a semaphore released per consumed frame.
- **No output passthrough module.** The game keeps the display card's adapter object (outputs, HDR desc, modes), only its device is created on the render card; `GetContainingOutput`, fullscreen, `ResizeTarget`, colour space and HDR metadata go to the real swapchain. Satisfactory kept its 3840x2160 window.
- **Devices DXGI, D3D12 and OptiScaler create for themselves are not redirected** (caller module check): redirecting DXGI's own device hung the cross-adapter present.
- **NR (M5) is whole frame first.** The presenter runs AMDNR's final image context (`dlssnr/amd/PresentExperimental.h`, lmxxf runtime, no-motion carry) on the display device and queue: the runtime binds HIP by LUID (`hip_device=0 (luid)`). The menu and the game's HUD go through NR. `Mode=hudless` is built in pass 2 (below).
- `[Mgpu]` keys: `Enabled`, `RenderAdapterBus`, `DisplayAdapterBus` (PCI bus, auto = the card without / with outputs), `Mode` (whole-frame / hudless), `Presenter` (false = Windows' cross-adapter present, the M3 test), `Fallback` (inline / off: what runs when no second card is found). Frame dumps: create `mgpu_dumps\mgpu_dump.trigger` beside the DLL (content: frame count); each frame gives `render` (render-card back buffer), `ingest` (display card after the copy) and `display` (after NR) as .raw + .bmp.
- `mgpu/tools/mgpu_harness`: a minimal D3D12 title (borderless window on the monitor, moving bars, frame counter strip, tearing present) that loads the build as `dxgi.dll`.

### Results (2026-11-04, display card bus 3 Gen5 x8, render card bus 7 Gen4 x8)

Satisfactory 1.2.4.1 main menu (live 3D scene, game cap 60 fps), 3840x2160 R10G10B10A2, FSR 4 through OptiScaler, no input:

| Run | Render card (bus 7) | Display card (bus 3) | Frame time | Send / ingest | Added latency | NR |
|---|---|---|---|---|---|---|
| Single card, NR off (baseline) | - | 3D 67 %, compute 22 % | 16.69 ms, Hardware: Independent Flip | - | - | - |
| Single card, NR inline (baseline) | - | 3D 65 %, compute 54 % | 32.5 ms mean, p99 62 ms, Composed: Flip | - | - | `hip_ms` 11.7, every frame |
| M3, Windows cross-adapter present | renders (copy engine 15 %) | 3D 7 % | game ran to its menu, clean exit | - | - | - |
| M4, proxy + ring, NR off | 70-100 % busy | 11 % busy | 16.67 ms, 0 skipped | 2.38 ms at 14.0 GB/s / 1.15 ms at 28.8 GB/s | about 5 ms | - |
| M5, NR on the display card | 70 % busy | 50-52 % busy | 16.67 ms, 0 skipped | same | about 5 ms | `hip_ms` 16.9, model every 2nd frame (300 per 600), record+submit 0.55 ms CPU |

- M4 image: 10 consecutive frames bit-identical between the render-card back buffer and the display card's back buffer (and different from each other).
- M5 image: render == ingest bit-exact; the presented frame differs (NR edit). The render card does not wait for NR: the game stays at its 60 fps cap where inline NR halves it.
- Harness (uncapped, 4K): M4 365-386 fps, bound by the render card's Gen4 x8 link (send 2.38 ms at 14 GB/s, ingest 1.17 ms at 28.5 GB/s, 2.5 ms added latency); M5 123 fps, bound by the display card (96 % busy: the host passes at 4K plus a 25.7 ms network every sixth frame).
- PresentMon could not confirm the present mode for the M4/M5 runs: from about 04:00 every PresentMon session on this machine (game or harness, filtered or not, realtime or from an ETL) reports lost events and writes no rows. The real swapchain matches the recipe that reached Hardware: Independent Flip in the NeuralHub spike: flip discard, tearing, a window covering its monitor, per-monitor DPI awareness (logged at proxy creation: Satisfactory's window is 3840x2160 at 0,0 on a 3840x2160 monitor, DPI awareness per monitor).

### Open

- PresentMon on the display card's swapchain (Independent Flip, VRR) still to be captured (done in pass 2, below).

### Pass 2 (2026-11-04): uncapped, HUD-less, temporal inputs

Code:

- **HUD-less source.** OptiScaler's HUDfix pick, without frame generation: with `[FrameGen] FGInput=upscaler` and FG output off, `Hudfix_Dx12` and `ResTrack_Dx12` treat the multi-GPU sink as their consumer (`MgpuHost::HudlessSink`); the upscaler Evaluate arms HUDfix for the frame (`MgpuHost::UpscaleEvaluated`) and the proxy's Present closes the window (`Hudfix_Dx12::PresentStart`). The pick is copied into a staging texture per game back buffer and tagged with the frame id; only same-format picks are taken.
- **Ring.** Each slot gains two regions after the back buffer: the HUD-less colour (same footprint) and the motion vectors (R16G16 float, up to the frame's size). The send list per slot is recorded at Present when the staging carries the frame's tag; constants (vector scale, jitter, reset, depth sense) travel in CPU memory with the slot.
- **UI composite** (`MgpuComposite`): NR runs on the HUD-less colour; `mask = smoothstep(2/1023, 16/1023, max3|bb - hudless|)`, `out = lerp(nr, bb, mask)` (mask 1 = the back buffer's texel, mask 0 = NR's). A group-shared count of `max3 > 2/1023` is read back per slot; above `UiGate` (percent, default 15) the frame runs whole-frame NR and the count is still taken, so the mode comes back when the share drops.
- **Temporal stage.** The runtime pair in use exports the fork ABI: `LMXXF_NR_FRAME_FLAG_TEMPORAL`, `motion` / `motion_scale_*` / `history_reset` / `output_smooth` in `LmxxfNrFrameInfo` (`dlssnr/lmxxf/runtime/LmxxfNrApi.h`), already driven by the backend's motion path inline. `[Mgpu] Temporal=true` sends the vectors and drives that path on the display card: display-sized unjittered colour, render-sized vectors (the backend resamples and rescales them), constant depth, history reset on a game reset, a vector size change or a gap over 250 ms.
- **`[Mgpu] NrEveryFrame`**: the presenter waits for the answer in flight before each no-motion frame (`Lmxxf::Backend::WaitAnswer`).
- **Uncapped menu**: Satisfactory caps its main menu with the console variable `FG.MaxMainMenuFPS` (help text in the game module: "FPS cap when in main menu. 0 = do not limit FPS in main menu"); the runs add `[SystemSettings] FG.MaxMainMenuFPS=0` to `Engine.ini` and restore it byte-exact.

Satisfactory main menu, uncapped, 3840x2160, FSR 4, no input, 30 s windows (PresentMon 2.6, 90 s warm-up):

| Run | fps | Frame time | Render card | Display card | Model runs | `hip_ms` |
|---|---|---|---|---|---|---|
| Single card, NR off | 72.4 | 13.81 ms (p99 15.0) | - | 3D 87 %, compute 28 % | - | - |
| Single card, inline NR, every frame | 37.5 | 26.66 ms | - | 3D 66 %, compute 56 % | 600 / 600 | 11.7 |
| Single card, inline NR, interleave 2 | 47.6 | 21.02 ms | - | 3D 74 %, compute 44 % | 300 / 600 | 11.5 |
| MGPU, NR off | 65.5 | 15.26 ms | 3D 88 %, copy 16 % | 3D 8 % | - | - |
| MGPU, NR (answer used when ready) | 65.4 | 15.29 ms | 91 % busy | 36 % busy, compute 37 % | 200 / 600 | 17.1 |
| MGPU, NR, interleave 2 | 65.6 | 15.26 ms | 90 % busy | 38 % busy | 200 / 600 | 17.7 |
| MGPU, NR every frame (`NrEveryFrame`) | 66.0 | 15.16 ms | 92 % busy | 66 % busy | 600 / 600 | 10.1 |
| MGPU, HUD-less (`UiGate=50`) | 49.8 | 20.08 ms | 92 % busy | 38 % busy | 200 / 600 | 19.1 |
| MGPU, HUD-less + temporal | 46.0 | 21.72 ms | 91 % busy | 69 % busy | 600 / 600, history 590 | 13.0 |

- **Present mode**: every run, single card and MGPU, is `Hardware: Independent Flip`, `AllowsTearing=1`, `SyncInterval=0`, `PresentFlags=512` (ALLOW_TEARING) on every frame; under MGPU the only swapchain PresentMon sees is the display card's real one. PresentMon had stopped writing rows because a realtime ETW session left by an earlier probe (`sat_base_next_probe`, no consumer) was still running; after `logman stop` it works unelevated.
- **MGPU vs inline**: the render card is the limit under MGPU (65 fps with NR off, 91 % busy; the render card on bus 7 is 10 % slower here than the display card running alone), and NR costs it nothing: 66 fps with the network on every frame, against 37.5 inline. With the answer used when ready the network runs every third frame at 65 fps (`hip_ms` 17 ms while it overlaps the ingest and host passes); waiting for it each frame brings `hip_ms` to 10.1 ms and every frame fresh at the same frame rate. The send queue lead makes the added latency 15 ms uncapped (5 ms at the 60 cap): the game runs two frames ahead of the display card.
- **HUD-less**: HUDfix's pick on this menu is the scene without the menu (dumps: `runs/p2b_hud50/dumps`, `hudless` vs `ingest`). The menu's two translucent panels make 34.9 % of the pixels differ, so the default 15 % gate falls back to whole frame (`runs/p2_hud`, logged `HUD-less gate: ... whole frame`); with `UiGate=50` the composite runs: in 10 dumped frames every UI pixel (2.35 M, `max3 > 16/1023`) equals the back buffer bit for bit and every scene pixel (5.37 M, `bb == hudless`) equals NR's output bit for bit (`dump_metrics.json`). Cost: the render card goes from 15.3 to 17.5 ms (resource tracking and two 33 MB copies), the display card's `hip_ms` from 17 to 19 ms.
- **Temporal**: the vectors arrive (2304x1296 R16G16 float, low-res, scale 2304/1296: UV units; 5308 of 5400 frames carried them) and the network runs with history (590 of 600 frames, 10 resets from the game's reset flag), but the composed output equals the HUD-less input bit for bit in every dumped frame while the backend reports an edit (mean 0.021): the motion path's result is not reaching the frame, so the flicker and ghost metrics of this run (both 0) measure nothing yet. No-motion HUD-less reference: flicker 0.00134 on input-static pixels (input change 0 there), ghost share 4.8 % of changed pixels (`runs/p2b_hud50/dump_metrics.json`). The menu camera moves (5.5 % of pixels static frame to frame, 2.8 % change by more than 0.05), so the menu itself is the moving sequence.

### Open

- Temporal stage: find why the motion path's result equals its input on the display card (candidates: the constant depth against the game's depth sense in the temporal pass, the residual-temporal setting, the post-placement layout `writeBack`); then the still / moving 10-frame gates with `dump_metrics.py`, and depth over the ring if the pass needs it.
- HUD-less gate: during a run the measured share jumps to 74.9 % or 100 % for a few frames (115-209 gate transitions per run), so some frames still pair a back buffer with another frame's pick; the gate catches them, the tag pairing needs a look. The Satisfactory menu needs `UiGate` above 35 %: whether the default stays 15 % is a decision.
- `hip_ms` under contention (17-19 ms with the answer used when ready, 10 ms when waited for); the render card's 2.2 ms HUD-less overhead.
- M6 passthrough items (HDR PQ / scRGB, exclusive fullscreen, resize while running) and the 10-minute stability run.
- The GPU Engine PDH counters report garbage for the render card's 3D and compute engines in typeperf (values above 10^9 %); the in-process sampler (`MgpuGpuLoad`) reads sane values.

## 11. The plugin version (`mgpu/plugin`)

On this branch the multi-GPU path is part of the lmxxf_nr plugin (`plugins/lmxxf_nr/src/mgpu/`, MIT) on plain upstream OptiScaler plus the plugin API extensions (`docs/PLUGIN_API_EXTENSIONS.md`): the device redirect is the adapter override (5), the proxy swapchain the swapchain provider (6), the HUD-less pick and the back buffer come from the present callback (2), the motion vectors from the upscale callbacks (1). `MgpuCore`, `MgpuSwapChain`, `MgpuComposite`, `MgpuGpuLoad` and `MgpuDump` are the `mgpu/terminal` pass-2 modules with three changes; `MgpuHost` is replaced by `MgpuPlugin.cpp`, and NR on the display card is the plugin's own engine (lmxxf's C API) instead of AMDNR's backend.

Changes against pass 2:

- **Pairing.** The HUD-less pick is handed over in the present of the frame it was captured in (`hudless` is NULL otherwise) and copied into the frame's staging right before the proxy's Present, on the game's queue. On the Satisfactory menu the measured UI share stays at 34.9 % (the menu's real coverage) for the whole run, one gate trip at the start, against 74.9 % / 100 % readings and 115-209 trips per run in pass 2.
- **Gate.** The UI share is not a reason for whole-frame NR any more: HUD-less mode never runs NR over the UI. `MgpuUiGate` (default 0.90) only detects a wrong pairing; then, and when a frame has no pick, the frame is presented without NR.
- **Frames ahead.** `MgpuMaxFramesAhead` (default 1, was 2 fixed).
- **NR on the display card.** The plugin engine on the display device and queue: with `MgpuNrEveryFrame` (default on) it feeds the frame, runs the network, waits for it on the GPU and applies this frame's edit; otherwise the previous frame's edit, moved by the frame's vectors. With a runtime that takes the temporal layout the network gets the vectors and keeps a history.

Satisfactory main menu, 3840x2160, FSR 4, no input, 30 s windows; display card bus 3, render card bus 7. "Steady" is the 10 s timeline in the middle of the window:

| Run | fps (window / steady) | Render card | Display card | NR | Added latency | Present |
|---|---|---|---|---|---|---|
| MGPU, NR off, uncapped | 60.6 / 63 | 15.9 ms, 93 % busy | 13 % busy | - | 15.4 ms | Hardware: Independent Flip |
| MGPU, whole frame, NR every frame, history | 59.7 / 63 | 16.0 ms, 94 % | 60 % busy | 9.6 ms (`hip_ms` 9.5), 600 / 600 | 15.6 ms | Hardware: Independent Flip |
| same, frames ahead 2 | 59.8 / 63 | 15.9 ms, 93 % | 59 % | 9.6 ms | 15.5 ms | Hardware: Independent Flip |
| same, history off | 59.6 | 15.9 ms, 93 % | 60 % | 9.5 ms | 15.4 ms | Hardware: Independent Flip |
| MGPU, HUD-less, NR every frame, history | (window composed) / 56 from the presenter | 17.8 ms, 93 % | 54 % | 9.7 ms | 17.4 ms | Composed: Flip (another window had the foreground) |

- The render card is the limit (63 fps with NR on or off); NR costs it nothing, as in pass 2 (65-66 fps there with two frames ahead).
- Added latency is the same with one or two frames ahead here (15.4-15.6 ms): it is the render card's time to finish the frame plus the send, not the proxy's queue.
- Dumps: render == ingest bit for bit; whole frame: the presented frame differs from the ingest on 99.9 % of the pixels, the network's edit mean 0.021 with history on and off. HUD-less: in 3 dumped frames every UI pixel (30.1 %, `max3 |bb - hudless| > 16/1023`) equals the back buffer bit for bit and every scene pixel changed.
