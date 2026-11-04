# AMDNR gaps in the build base

Branch `mgpu/base`. This file lists every piece of AMDNR that the build base does not have, what stands in for it, and what to do when AMDNR publishes its full source (its README says: with 0.5.0).

This fork is a modified AMDNR. It is not the original AMDNR and is not endorsed by its author. The credit for AMDNR goes to 3zwr1 (https://github.com/3zwr1/AMD-NR---OptiScaler).

## 1. Layout

| Path | What it is |
|---|---|
| `src/` | Upstream OptiScaler `c983a500335134ecff512bfcdadcf912d1286547` (2026-09-01), added as a squashed `git subtree` from the `optiscaler` remote (https://github.com/optiscaler/OptiScaler). Its submodules are registered in the top-level `.gitmodules` at `src/external/*`. |
| `src/OptiScaler/` on top of that | AMDNR's published 0.3.4.2 sources, ported as a verbatim commit ("Port AMDNR 0.3.4.2 published sources ..."), then the build and gap commits. |
| `main/AMDNR-source/` | AMDNR's snapshot, untouched, for reference and for diffing against AMDNR 0.5.0. |
| `third_party/directx_agility_sdk/` | Agility SDK 1.615.1 headers (MIT), see 4. |
| `mgpu/scripts/` | Build, header fetch, literal split, game backup / install / revert. |

Why a subtree under `src/` and not OptiScaler's history as the branch root:

- The repo root stays AMDNR's release repo, so `git merge upstream/main` (3zwr1) keeps working with shared history, and `main/AMDNR-source` stays where AMDNR puts it.
- Upstream OptiScaler updates come in with `git subtree pull --prefix=src --squash optiscaler <commit>`. The squash commit message records the exact upstream commit (`git-subtree-split:`).
- AMDNR's code is a separate commit on top of the upstream tree, so when AMDNR 0.5.0 lands, its tree can be diffed against `c983a500` + the port commit, and the gap commits below can be dropped one by one.

Why `c983a500`: it is the upstream commit closest to the 0.3.4.2 snapshot by blob match, so the 98 upstream files AMDNR modified apply as whole files without a three-way port.

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
| `spoofing/Dxgi_Spoofing.cpp` (upstream, not modified by AMDNR's snapshot) | Upstream `8f2a9435` (2026-10-23) backported: `GetDesc1` keeps the real AMD vendor id for `amdxcffx64.dll`. Without it the driver's FSR 4 provider sees the spoofed NVIDIA adapter and refuses, and the FFX Upscaler list has only 3.1.6 and 2.3.4. | 0.3.5.8R13 lists four providers (SDK `4.1.2`, driver `4.1.2 *`, `3.1.6`, `2.3.4`) because it lists the driver provider next to the SDK's itself (the newer upstream `FSR4DriverProvider`). This base lists three: `4.1.2 *`, `3.1.6`, `2.3.4`, so an `[FSR] UpscalerIndex` saved by R13 points one entry further down here (R13's 1 = `4.1.2 *`, here 0). |
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
