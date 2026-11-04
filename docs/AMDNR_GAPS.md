# AMDNR gaps in the build base

Branch `mgpu/base`. This file lists every piece of AMDNR that the build base does not have, what stands in for it, and what to do when AMDNR publishes its full source (its README says: with 0.5.0).

This fork is a modified AMDNR. It is not the original AMDNR and is not endorsed by its author. The credit for AMDNR goes to 3zwr1 (https://github.com/3zwr1/AMD-NR---OptiScaler).

## 1. Layout

| Path | What it is |
|---|---|
| `src/` | Upstream OptiScaler `5eb4bfabf3c93cdb7e96b37065441f5c8000ccc7` (2026-11-03), a squashed `git subtree` from the `optiscaler` remote (https://github.com/optiscaler/OptiScaler). First added at `c983a500` (2026-09-01), then pulled to `5eb4bfab` (section 6). Its submodules are registered in the top-level `.gitmodules` at `src/external/*`. |
| `src/OptiScaler/` on top of that | AMDNR's published 0.3.4.2 sources, ported as a verbatim commit ("Port AMDNR 0.3.4.2 published sources ..."), then the build and gap commits. |
| `main/AMDNR-source/` | AMDNR's snapshot, untouched, for reference and for diffing against AMDNR 0.5.0. |
| `third_party/directx_agility_sdk/` | Agility SDK 1.615.1 headers (MIT), see 4. |
| `mgpu/scripts/` | Build, header fetch, literal split, game backup / install / revert. |

Why a subtree under `src/` and not OptiScaler's history as the branch root:

- The repo root stays AMDNR's release repo, so `git merge upstream/main` (3zwr1) keeps working with shared history, and `main/AMDNR-source` stays where AMDNR puts it.
- Upstream OptiScaler updates come in with `git subtree pull --prefix=src --squash optiscaler <commit>`. The squash commit message records the exact upstream commit (`git-subtree-split:`).
- AMDNR's code is a separate commit on top of the upstream tree, so when AMDNR 0.5.0 lands, its tree can be diffed against `c983a500` + the port commit, and the gap commits below can be dropped one by one.

Why `c983a500` first: it is the upstream commit closest to the 0.3.4.2 snapshot by blob match, so the 98 upstream files AMDNR modified apply as whole files without a three-way port. Diffs that isolate AMDNR's own changes to upstream files are still taken against `c983a500` (`git diff c983a500:OptiScaler/<f> <branch>:src/OptiScaler/<f>`).

The subtree carries upstream's own binaries (prebuilt FFX `.lib` files, `dxcompiler.dll`, `D3D12Core.dll`, `freetype.lib`) because the build links them. They are upstream's, unchanged. No AMDNR, lmxxf or NVIDIA binary is in the tree.

## 2. Recovered from AMDNR's published source (compiled as published)

Everything in `main/AMDNR-source` that compiles on this base is built unchanged, including:

- `dlssnr/amd/` (AmdBridge, AmdPreSr, the host passes, the colour encoding, temporal stability, interleave, NR compose);
- `dlssnr/lmxxf/` (LmxxfBackend, the tier policy, the runtime C ABI `runtime/LmxxfNrApi.h`, ABI v1);
- `dlssnr/menu/` and `DlssNr_Menu.cpp` (the Neural tab), `dlssnr/gi/` (AMDNR Screen GI, preview, off by default);
- the `[DlssNr]` config block (`Config.h` / `Config.cpp`), the toggle and capture keys;
- AMDNR's changes to upstream files that it published (98 files: hooks, menu, framegen, hudfix, resource tracking, upscalers, Streamline hooks, etc.).

Edits to published AMDNR files: the raw-literal split in `dlssnr/amd/TemporalStability.h` (4.3), and the 0.3.5 looks and placement of 3.4 (`Config.h/.cpp`, `dlssnr/amd/AmdBridge.cpp`, `dlssnr/lmxxf/LmxxfBackend.cpp`, `dlssnr/menu/NeuralTop.cpp`; the post placement's log lines say "after upscaling" where 0.3.4.2 said "after Ray Regeneration", its only post case).

## 3. Gaps and what stands in for them

### 3.1 AMDNR-only files that are not published

| Missing file | Stand-in | Effect |
|---|---|---|
| `shaders/dlssnr/DlssNr_Dx12.cpp` (the NVIDIA `nvngx_dlssnr` pass, feature lifetime, the evaluate dispatcher, `ScopedNrStateEnvelope`) | Replacement at the same path: the `DlssNr` namespace API routes to `DlssNr::AmdBridge`. `EvaluateBeforeUpscale` runs `AmdBridge::Before` (and the GI seam when `[AmdGi]` is on) when the placement is pre; `EvaluateAfterUpscale` runs `AmdBridge::After` when it is post, and always after an RR evaluate. The placement is `[DlssNr] AmdPlacement`, or the NR look's own (3.4). `RunBeforeSR` / `ApplyAfterRR` are the NVIDIA path's keys and do not steer the AMD path, as in 0.3.4.2. | AMD path only. The NVIDIA model path is off. No `ScopedNrStateEnvelope`: the NR passes sit inside the upscaler's own root-signature window (upstream `RestoreRoot`, controlled by `RestoreComputeSignature` / `RestoreGraphicSignature`). The presented-frame epoch is passed as 0 (only `AmdOneStreamPerFrame` reads it). Calibration, exposure status, captures and `LastGpuTime` of the NVIDIA path return empty values. |
| `shaders/dlssnr/DlssNr_Common.h` | Stub with the standard headers the API headers need. | None for the AMD path. |
| `shaders/dlssnr/DlssNr_Vk.cpp/.h`, `DlssNr_ActiveColor.h`, `precompile/dlssnr.hlsl` | `DlssNr_Vk.cpp` dropped from the project; `dlssnr/DlssNrFeature_Vk.cpp` excluded; the Vk API is stubbed in the replacement above. | No native Vulkan NR. The Vulkan-on-D3D12 bridge path (`IFeature_VkwDx12`) still calls the AMD bridge. |
| `shaders/fsrd_preprocess/*` (`FSRDPreprocessor_Dx12`, `FSRDShaderUtils.h`, `FSRDRuntimeStatus.h`) | `FSRDRuntimeStatus.h` stub (every value "no RR frame yet"); the rest absent. | FSR Ray Regeneration cannot build: see 3.3. |
| `nvsdk_ngx_defs_dlssd.h` (AMDNR's NGX header) | none | Only `FSRDFeature_Dx12.cpp` needs it (excluded). |
| `proxies/XeFGPacing.h` (pacing engine, MFG unlock) | Stub: `XeFGPacing` and `XeFGUnlock` no-ops, `UnpacedMaxInterpolations = 5`. | XeFG runs as upstream: no pacing engine, 6X at most, no unlock of the game's own provider. |

### 3.2 Upstream files AMDNR modified but did not publish

232 of the project's items come from upstream `c983a500` unmodified (`inputs/` 44, `shaders/` 128, `include/` 25, `low_latency/` 21, `proxies/` 12, `scanner/` 2). Where AMDNR's code needs their AMDNR versions, the base carries minimal adaptations:

| File | Change | AMDNR's version did more |
|---|---|---|
| `inputs/NVNGX_DLSS_Dx12.cpp` | The NR call sites around `feature->Evaluate` in `TryEvaluateOptiFeature`: `EvaluateBeforeUpscale`, `Evaluate`, `AmdBridge::Restore`, then `EvaluateAfterUpscale` (or `AmdBridge::UpscalerSkipped` on a failed evaluate). `rayReconstruction` is set on the context. | `dlssnr/README.md` names two evaluate routes; upstream `c983a500` has one. AMDNR's RR fallbacks (FSR 3.1/4, FSR 2.1.2), the `RayRegenOffForSession` reads and the bias-mask routing are not there. |
| `inputs/NVNGX_DLSS.h` | `ContextData::rayReconstruction`. | |
| `inputs/FG/Upscaler_Inputs_Dx12.cpp` | `CaptureInfo::Upscaler` -> `UpscalerPass` (AMDNR renamed the enumerator). | |
| `proxies/XeFG_Proxy.h` | `NativeModule`, `NativePath`, `NoteInitResult`, `InitFailedAt`, `InitFailedCode` stubs. | Native-XeFG detection (a game's own `libxess_fg.dll` keeps its own FG) and the init-failure readout. |
| `proxies/FfxApi_Proxy.h` | `IsDenoiserApiImplementedDx12()` returns false, `DenoiserApiGenerationDx12()` returns `Unsupported`, enum `FfxDenoiserApiGeneration`. | The Ray Regeneration 1.2 ABI check. |
| `spoofing/Dxgi_Spoofing.cpp` (upstream, not modified by AMDNR's snapshot) | Upstream's file. `mgpu/base` backported `8f2a9435`; `mgpu/base-next` has it from upstream (6.2): `GetDesc1` keeps the real AMD vendor id for `amdxcffx64.dll`, so the driver's FSR 4 provider does not refuse the spoofed adapter. The FFX Upscaler list is now R13's: `4.1.2` (SDK), `4.1.2 *` (driver, upstream `FSR4DriverProvider`), `3.1.6`, `2.3.4` (Satisfactory probe log, `FFX upscaler provider 0..3`). An `[FSR] UpscalerIndex` saved by R13 means the same entry here again; one saved by `mgpu/base` (0 = `4.1.2 *`) now selects the SDK `4.1.2`. | |
| `shaders/output_scaling/OS_Dx12.cpp` | none (upstream) | AMDNR's dispatch-size fix (`dlssnr/README.md`): only matters when NR calls Output Scaling, which the AMD path does not. |
| Everything else in `inputs/`, `proxies/`, `shaders/`, `low_latency/` | upstream | Unknown until AMDNR publishes; diff the 0.5.0 tree against `c983a500`. |

### 3.3 Features stubbed off

| Feature | Why | Build state |
|---|---|---|
| FSR Ray Regeneration (`Upscaler::FSR_RR`, `FSRDFeature_Dx12`, `FSR31Feature_Dx12`) | Needs `FSRDPreprocessor_Dx12` and `nvsdk_ngx_defs_dlssd.h`. | Excluded from the build; `FeatureProvider_Dx12` creates FFX for an RR request. The Neural menu's RR rows show "no RR frame". |
| NVIDIA `nvngx_dlssnr` path, `dlssnr_forwarder` | Needs `DlssNr_Dx12.cpp`. Out of scope on AMD. | The forwarder project stays in the solution but is not built by `build.ps1`. |
| Native Vulkan NR | Needs `DlssNr_Vk`. | Excluded. |
| XeFG pacing / MFG unlock | Needs `XeFGPacing.h`. | No-ops. |
| `DlssNr::ExposureScan` feed | Called from the NVIDIA path. | The menu's exposure readout stays empty; lmxxf reads the title's exposure texture itself. |

### 3.4 Behaviour that is 0.3.5.x, not 0.3.4.2

The base is the 0.3.4.2 snapshot; the released binary is 0.3.5.8R13. Rebuilt from AMDNR's published 0.3.5.8 docs (README "NR style" / "Neural placement", the release `OptiScaler.ini`) as parameter presets over keys and runtime controls 0.3.4.2 already has (`dlssnr/amd/NrLooks.h`):

| 0.3.5 item | Here |
|---|---|
| `[DlssNr] AmdNrLook`: Full Model Look (`auto`), NVIDIA look (`0`), AMDNR soft (`1`), first in the NR style combo, the named styles (Cinematic, Crisp, Natural, Vivid) after them | Done. A look fills only keys left on auto, as volatile values (never saved): Structure intensity, Edit detail, Edit colour, Residual limit, Edge guard, Temporal stability, Output smoothing, `AmdRuntimeStyle`, `LmxxfFullNetwork`. Picking one in the menu resets those sliders and writes its placement and Full network as the user's values, as R13 does. |
| lmxxf network style (`AmdRuntimeStyle` 0..2 on lmxxf) | Done: sent as the ABI v1 `control_style` (0.3.4.2 always sent the shipped 1). The controls match R13's log: Full Model Look `ctl=1,1,3,2,0`, NVIDIA look with `AmdPlacement=pre` `ctl=1,1,1.5,1,0`. |
| `[DlssNr] AmdPlacement` (pre / post / empty = the look's) | Done, plus a Neural placement combo next to NR style. 0.3.4.2 ran the AMD path pre only; post came with 0.3.5. |
| `[DlssNr] AmdFaceDetail` (faces 1.5x the structure while Character structure follows it, at most 4) | Done (`AmdBridge` BuildSettings). |
| `AmdLmxxfFreshPrior`, fresh-only carry (`[LMX-CARRY]`), `[LMXHIST]` history tests, `[POSTLOOK] mv-resample=nearest`, `AmdDetailLift` wording, the TDR record / cost gate that moves post back to pre, the 2560x1440 network size under the NVIDIA look (`AmdLmxxfTierCap`), Refine passes, NR resolution above 150 %, HIP cache off (`GPU_RESOURCE_CACHE_SIZE=0`) | Not recovered: host or runtime code that the published docs describe but do not specify. Wait for the 0.5.0 source. |

`LmxxfFullNetwork` exists in both.

## 4. Toolchain

1. `v145` -> `v143` (VS 2022). Nothing in the code needs v145 beyond 4.3.
2. Windows SDK 10.0.22621 lacks `D3D12_STATIC_SAMPLER_DESC1` and root signature 1.2, which upstream `D3D12_Hooks.cpp` uses. The Agility SDK 1.615.1 headers (the version of upstream's `D3D12Core.dll`) are first on the include path. `mgpu/scripts/fetch_agility_headers.ps1` re-fetches them from nuget.org with a pinned SHA-256.
3. MSVC v143 refuses a single string literal over 16 KB (C2026). `TemporalStability.h` holds a 142 KB HLSL raw literal; `mgpu/scripts/split_raw_literals.py` cuts it into adjacent literals at line breaks. The text the shader compiler sees is unchanged. Re-run it after merging an AMDNR update.
4. `mgpu/scripts/build.ps1` builds `src/OptiScaler.sln` target `OptiScaler`, Release x64, with `/m` and `CL_MPCount` = logical threads - 2. The DLL lands in `src/x64/Release/a/OptiScaler.dll`.

## 5. When AMDNR publishes its full source

1. Add its tree as a new snapshot next to `main/AMDNR-source` (or take `upstream/main`).
2. Diff it against `c983a500` + the port commit; take its versions of every file in 3.1 and 3.2, and drop the stand-ins (each carries an "AMDNR gap" banner; `git grep "AMDNR gap"` lists them).
3. Re-enable the excluded project items (`ExcludedFromBuild`) and restore the dropped `ClCompile` entries.
4. If it is based on a newer OptiScaler, `git subtree pull --prefix=src --squash optiscaler <its base>` first.

## 6. Upstream sync

Branch `mgpu/base-next` is `mgpu/base` plus `git subtree pull --prefix=src --squash optiscaler 5eb4bfab`. That is upstream master of 2026-11-03, 335 commits after `c983a500`.

### 6.1 Method

- Every conflicted file was re-merged three-way with `c983a500` as the base.
  - Ours is `mgpu/base` (AMDNR's port plus the gap commits). Theirs is `5eb4bfab`.
  - Each hunk then shows AMDNR's delta (`c983a500` -> ours) next to upstream's (`c983a500` -> `5eb4bfab`).
  - The resolution is upstream's file with AMDNR's delta applied again.
- A newer upstream commit was tried as the base where AMDNR's file is closer to it than to `c983a500`.
  - It was dropped. It reads upstream's later changes, which AMDNR does not have, as AMDNR deletions. It lost `FrameLimit::targetFps` and `ContextData::lastEvalFrame`.
  - So `c983a500` stays the base for every file.
- 55 files conflicted, about 270 hunks (`menu/menu_common.cpp` alone had 127). Files that merged cleanly were checked by the build and by a search for duplicate definitions.
- Where upstream fixed something AMDNR had already fixed, upstream's code is taken and AMDNR's is dropped. 6.3 lists those cases.

### 6.2 Dropped from our commits

| Commit | Why |
|---|---|
| `639853fb` (FSR 4 spoof backport) | Upstream now contains `8f2a9435`. `spoofing/Dxgi_Spoofing.cpp` is upstream's file again. |

### 6.3 AMDNR code replaced by upstream's version of the same feature

| AMDNR | Upstream | Decision |
|---|---|---|
| `DLSSFeatureDx11on12` (`upscalers/dlss/DLSSFeature_Dx11On12.*`) and `DLSSFeatureVkOn12` (`DLSSFeature_VkOn12.*`) | `DLSSFeatureDx11On12` and `DLSSFeatureVkOnDx12` (`435609d4`), with the `Upscaler::DLSS_on12` cases in both providers | Upstream's classes and provider cases are kept. AMDNR's duplicate `case DLSS_on12` blocks, its two `VkOn12` files and their project entries are removed. Upstream also leaves `SupportsUpscaleSize` unset on these classes, which was AMDNR's point. |
| `DLSSFeature::_dlssInitedDx11/Dx12/Vk`: one static flag per API | `_dlssInited` is a per-instance member | Upstream's. It fixes the same shared-flag bug, where a D3D11 init hid the D3D12 one. |
| `FrameLimit::sleep(fgActive, outputPerBase)`: the FG multiplier passed in | `sleep(fgActive)` reads `CurrentFG()->GetInterpolatedFrameCount() + 1`. Upstream also adds `targetFps()` (Dynamic MFG). | Upstream's. The call in `FG_Hooks.cpp` goes back to one argument. AMDNR's `sleep_ns` / `now_ns` stay, because `InterleavePacing.h` uses them. They now run on `Util::GetTimestamp`. |
| `Util::IsSubpath`: never throws, empty result counts as inside | Lexically relative, case-insensitive, never throws, empty result counts as not inside | Upstream's. AMDNR's comment asked for an audit of the callers before the empty case changed; upstream has now made that change. |
| No automatic FSR 4 INT8 on integrated RDNA 3 (`IdentifyGpu.cpp`) | Per-ASIC model table. Phoenix, Hawk Point and Krackan get a model on purpose. | Upstream's. AMDNR's exclusion worked around a crash (SOTTR, 780M) in the 0.3.x driver path. `[FSR] Fsr4ForceModel` went away with upstream's FSR 4 rework. |
| `AmdVkLateCopyWait`: Vulkan bridge, opt-in, off by default | The bridge records every copy-back step that can fail before it queues the D3D12 Wait (`AbortPendingInterop`, `CancelAndDrainQueuedWait`) | Upstream's. The hold / flush / drop code is removed.<br>The `[DlssNr] AmdVkLateCopyWait` key still parses and the Neural tab still mentions it, but it has no effect.<br>`AmdBridge::ListDiscarded` is now called from `AbortPendingInterop` and from the failed-Close path, so the NR backend still hears about a discarded bridge list. |
| `FSR4Upgrade.cpp` denoiser log line (P25: "no driver override for the denoiser" logs as INFO, not ERROR) | `FSR4Upgrade.cpp` is deleted; the driver provider moved to `fsr4/FSR4DriverProvider.cpp` (`37c40ef7`) | Upstream's. The log-level change is not applied again. |
| `FSRFG_Dx12.cpp`: log text "FSR FG swapchain already created" | Upstream removed the block | Upstream's. |
| `Vulkan_Hooks.cpp`: surface -> window map (`_surfaceHwnd`, AMDNR 0.3.4.1) | `_surfaceWindows`, `VulkanHooks::SurfaceWindow`, `OnOtherWindow` | Upstream's. It fixes the same vkd3d-proton helper-window problem. |
| `Streamline_Hooks.cpp`: system caps pointer read again on every call (0.3.3 F6) | The same fix, plus an `IsWritableMemory` check | Upstream's. |
| `input_system_raw.cpp`: "a repeat is not a blocked press" | The same fix | Upstream's. |
| `input_system_windows_hooks.cpp`: low-level hook pass-through | Upstream now always passes low-level hook events through | AMDNR's is kept. It has the same default, plus the `[Hotfix] MenuLowLevelHookPassThrough` kill switch and a log line for the first blocked event. |
| Product name in `resource.h` (`VER_PRODUCT_NAME`) | Moved behind `OPTISCALER_BUILD_METADATA` and read through `BuildInfo::ProductName()` | AMDNR's name ("AMD-NR v... / OptiScaler v... (build)") is kept inside the guard. `AmdBridge.cpp`, `Logger.cpp`, `CrashReport.cpp` and `dllmain.cpp` now call `BuildInfo::ProductName()`. |
| Quirk table in `misc/Quirks.h` | Moved to `misc/GameConfigs.cpp` (`a45a18b5`) and split into config defaults and quirks | Upstream's CONTROL Resonant entry is taken as is. It sets `SimulateWaitableObject` and `DisableOTA`, and no longer sets the `FGPreserveSwapChain` default (upstream "Remove PreserveSwapChain config default from Control Resonant").<br>AMDNR's enum members are appended to upstream's enum: `PathTracedRayReconstruction`, `SkinSmoothingDefault`, `RrBiasMaskDefaultOff`, `NeedsREFramework`.<br>AMDNR's table edits become new `GameConfigs.cpp` entries:<ul><li>RE Requiem (and its demo): its quirks.</li><li>PRAGMATA: `NeedsREFramework`.</li><li>`gta5.exe`: `SkipD3D11FeatureLevelElevation`.</li><li>Stellar Blade: `OnNonNvidia(RestoreComputeSignature = true), ExtendedStateRestore = true`.</li></ul>Onimusha's entry is already upstream. |

### 6.4 AMDNR deltas applied again on upstream's new code

| File | Decision |
|---|---|
| `inputs/NVNGX_DLSS_Dx12.cpp` | The NR call sites (`AmdBridge::Restore`, then `EvaluateAfterUpscale` or `UpscalerSkipped`) sit before upstream's new `UpscalerInputsDx12::UpscaleEnd`, so FG and HUDfix resource tracking see the NR output. |
| `inputs/NVNGX_DLSS.h` | `rayReconstruction` sits next to upstream's new `lastEvalFrame`. |
| `upscalers/FeatureProvider_Dx12.cpp` | AMDNR's rule that an RR handle keeps its backend on Reset runs after upstream's custom-DLSS check (`CustomProviders::IsDlss`). |
| `upscalers/ffx/FFXFeature.cpp`, `FFXFeature_Dx12.cpp` | Upstream moved the version lists to `State::ffxUpscalerVersionsDx12` and to the custom FFX modules. AMDNR's provider log (`FFX upscaler provider N: name (id)`) and its `[CREATE]` lines are added again on the new structures.<br>Upstream removed the FSR 4 autoexposure padding workaround when it replaced FSR 4's autoexposure with its own. AMDNR's barrier fix lived inside that workaround, so it is gone too. |
| `upscalers/IFeature_VkwDx12.cpp` | Upstream's reworked interop: async copy-back, parameter restore, `StartInterop` timing. AMDNR's NR calls stay inside the evaluate. |
| `upscalers/IFeature_Dx11wDx12.cpp` | AMDNR's command-list recreation after a failed Close uses upstream's `commandFrame` index. |
| `hooks/D3D12_Hooks.cpp` | The `DiagSwitches::NoD3D12Hooks` early return comes before upstream's `o_earlyHook` check. |
| `hooks/Ntdll_Hooks.h` | In `LdrLoadDll`, both upstream's `AfterLoadLibrary` and AMDNR's `AfterLoad` run. AMDNR's is the XeFG unlock, a no-op in this base. |
| `hooks/LibraryLoad_Hooks.cpp` | `XeFGUnlock::ApplyToAllLoaded` and the hook for an already-loaded `sl.dlss_d` are kept, next to upstream's moved `isLocalStreamlineModule`. |
| `hooks/DxgiFactory_Hooks.cpp` | AMDNR's window-sized swapchain resolution (`ResolveWindowSizedSwapchain`) runs on a local copy, then upstream's size check, which allows a zero size. |
| `nvapi/NvApiHooks.cpp` | AMDNR's external-FG pass-through comes first, then upstream's Reflex Sync hook. |
| `hooks/Streamline_Hooks.cpp` | Upstream's per-copy plugin slots (`pluginSlots`, `hookPlugin`, `onModuleFreed`) and its DLSS-G state fill are taken.<ul><li>AMDNR's 6X cap (`DlssgReportedMaxGenerated = 5`) clamps both the maximum and the presented count.</li><li>AMDNR's `externalFrameGeneration` / `DiagNoStreamlineHooks` guards moved into `hookPlugin`.</li><li>AMDNR's `PatchPluginJson` is kept over upstream's `HasPluginJson`, because it fails closed.</li><li>AMDNR's `sl.dlss_d` hooks stay outside the slot system with a single detour, so they lack upstream's protection against several loaded copies.</li><li>Tags are read on D3D12 and Vulkan (upstream). AMDNR's RR probes stay D3D12-only.</li></ul> |
| `hooks/Vulkan_Hooks.cpp` | Upstream's rewritten `vkCreateDevice`, with AMDNR's `SkipOwnVulkanObjects` handling applied again.<br>New in neither side: OptiScaler's own devices also get no FG, which this combination of the two needs.<br>The NR NVX device extensions are added before upstream's spoof scope. |
| `menu/input/*` | AMDNR's diagnostics (`DiagHookGroup`, the skip switches), held-key handling and event-time plumbing are applied again on upstream's code: its blocking policy (`ShouldBlock*InputLocked`, `HandleBlockingFocusGainLocked`), the DirectInput per-vtable trampolines and `USE_HID_HOOKS`. |
| `framegen/xefg/XeFG_Dx12.cpp` | AMDNR's `XeFGPacing` calls (no-ops in this base) sit next to upstream's Dynamic MFG restart and `release_xefg_xell`. The `createdHere` failure cleanup stays; upstream moved the "swapchain already exists" block out. |
| `dllmain.cpp` | AMDNR's native XeSS FG guard runs after upstream's XeFG input passthrough. |
| `wrapped/wrapped_swapchain.cpp` | In `SetColorSpace1`, AMDNR's colour-space log runs after upstream's HDR state and theme update. |
| `State.h`, `Config.h`, `Config.cpp`, `OptiScaler.ini` | Both sides' members and keys are kept.<br>`[XeFG] InterpolationCount` has no upper bound (upstream); AMDNR's `MaxInterpolatedFrames` keeps its own.<br>The process exclusion list has both sides' additions. |
| `misc/AmdnrReport.cpp`, `dlssnr/amd/PresentExperimental.h` | Use upstream's new names: `dxgiIsDxvk`, `d3d12IsVkd3dProton`, `hdrOutputActive`. |
| `OptiScaler.sln`, `.vcxproj`, `.filters` | Upstream dropped the Win32 configurations, so the `dlssnr_forwarder` project no longer has them either. |
| `menu/menu_common.cpp` | Upstream and AMDNR redesigned the main menu separately (upstream: sidebar tabs, cards, Custom and plugin tabs, theme rework; AMDNR: its own palette and `NeuralUi` widgets), and diff3 gave 127 hunks that did not line up. The file was rebuilt from upstream and AMDNR's delta applied again function by function.<ul><li>About 130 of AMDNR's 224 delta hunks only restyled upstream sections (palette, `NeuralUi` widgets in upstream sections, status pills, footer graph). Upstream's look is kept and those are dropped.</li><li>The Neural tab is an upstream sidebar tab right after Custom and draws `DlssNr::RenderMenu`. `MenuCommon::RequestTab` maps to upstream's tab labels.</li><li>Ported: the menu-key latch and toggle debounce; the NR toggle and capture keys; the toggle toast; the first-launch runtime chooser; the REFramework warning; the exposure-scan lamp; the NR compare tags on the overlay; the Save report row; AMDNR credits and update notice; the " - Neural" suffix in the upscaler combo; a Neural Rendering keybind row; a Frame Gen "Compatibility (external FG)" box; the Root Signatures note.</li><li>Not ported, because the backing code is stubbed in this base (3.3) or upstream replaced it: the FSR-RR menu rows; the XeFG 7X-10X ceiling, unlock and pacing rows; AMDNR's V-Sync, render-resolution and init-flag sections (upstream has its own); the NVIDIA NR GPU-time row.</li><li>The build line shows `BuildInfo::ProductVersion()`, because upstream keeps `VER_BUILD_DATE` / `VER_BUILD_COMMIT` in one translation unit.</li></ul> |
| `menu/menu_common.h` | Upstream's header (it keeps `RenderThemeSettings`, which the auto-merge had dropped), plus AMDNR's declarations. `ScopedCollapsingHeader` is upstream's, so the Neural tab's folds look like upstream's. |
| `menu/menu_overlay_dx.cpp` | Both sides fixed command allocator reuse. Upstream's fence wait (up to 2 s) is kept; AMDNR's skip-when-busy round robin is dropped. AMDNR's overlay timing warning stays, and its skip counter now counts upstream's wait failures. |
| `menu/menu_overlay_vk.cpp` | AMDNR's Proton helper-window fallback (`ResolveMenuWindow`) runs first, then upstream's `OnOtherWindow` check. |

### 6.5 Toolchain

- Upstream's `external/directx_agility_sdk` is unchanged (`D3D12Core.dll` 1.615), so the vendored Agility SDK 1.615.1 headers stay as they are.
- Submodule changes: `FidelityFX-SDK-v2` -> `7539ae8b` (v2.4.0) and `vulkan` -> `8864cdc8`.
- `TemporalStability.h` is unchanged, so `split_raw_literals.py` did not need to run.

### 6.6 What users get from the newer upstream

- FFX SDK 2.4 with per-feature DLLs. When a per-feature DLL is missing, the older SDK DLL is used.
- FSR 4 rework:
  - the driver provider is listed next to the SDK one (the `*` entry);
  - live switching between the FP8 and INT8 models, with the model picked automatically per ASIC;
  - FSR 4 upscaling and FG on Vulkan, and FSR 4 swapchain upgrades on D3D12;
  - FSR 4 paths for Pascal and RDNA 1 / 2.
- Custom upscaler and FG DLLs (`custom/<name>/ffx|ffx_vk|dlss|dlssg`) and the Plugin API: ASI plugins and custom DLLs get a menu tab, settings and logging. Assessment: `docs/PLUGIN_API_ASSESSMENT.md`.
- Frame generation: XeFG Dynamic MFG; OptiFG, DLSSG and FSR FG on Vulkan; reprojection; a rework of low-latency input and output.
- FPS overlay presets, reworked frametime reporting, and the upscale ratio on the overlay.
- Per-game defaults are split from quirks (`GameConfigs.cpp`). CONTROL Resonant gets `SimulateWaitableObject`, OTA off and no `FGPreserveSwapChain` default.
- Fixes to GPU detection and FSR 4 availability, plus many crash fixes: DLSSG on Vulkan and D3D11, RTSS, swapchains with more than 8 buffers, and two D3D11-on-12 upscalers at once.

### 6.7 Verification

- Build: `mgpu/scripts/build.ps1` (VS 2022 v143, Release x64) links `OptiScaler.dll` with no errors.
- Probe (Satisfactory, D3D12, no input, the main menu scene, about 2 minutes, the previous `dxgi.dll` restored byte-exact afterwards):
  - FFX upscaler providers 0..3: `4.1.2`, `4.1.2 *`, `3.1.6`, `2.3.4`.
  - lmxxf: `modules_ok=208`, HIP device matched by LUID, network 1920x1080, `hip_ms` mean 11.25 (max 15.7) over the first 600 frames.
  - `AMD neural: NR look Full Model Look, placement after upscaling (the look)`.
  - Clean exit (`Clean exit recorded`), no crash.
- A second probe added the plugin PoC (`mgpu/plugin-poc`) in `OptiScaler\plugins\` with `[Plugins] LoadAsiPlugins=true`.
  - It loaded and registered its tab.
  - It logged every DX12 upscaler evaluate's inputs: colour 2304x1296, output 3840x2160, depth, MVs, exposure, jitter, MV scale.
  - Alongside it, the fork's own NR ran unchanged: `hip_ms` mean 11.68.
