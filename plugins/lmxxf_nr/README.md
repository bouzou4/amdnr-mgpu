# lmxxf_nr: DLSS 5 Neural Rendering for OptiScaler, as a plugin

An OptiScaler ASI plugin that runs lmxxf's DLSS 5 Neural Rendering runtime (`LmxxfNrRuntime.dll`, lmxxf's MIT C API `LmxxfNrApi.h`) around the upscaler or at present, on one GPU or on a second one. MIT licence (`LICENSE`).

It needs an OptiScaler with the plugin API extensions of this repository (`docs/PLUGIN_API_EXTENSIONS.md`): upscale callbacks (1) for the before / after placements, overlay text (3), the present callback (2) for the terminal placement and the HUD-less capture, and the adapter override (5) + swapchain provider (6) for multi-GPU. Each feature checks its API field and stays off without it.

## What the user installs

A drop-in add-on to an OptiScaler release. Extract OptiScaler into the game, then add `OptiScaler\plugins\lmxxf_nr.asi` and `OptiScaler\plugins\lmxxf_nr_profiles.ini`, then run OptiScaler's `setup_windows.bat`. With a one-line setup change (drafted for upstream; this repository's `src/setup_windows.bat` carries it), setup sets `LoadAsiPlugins=true` when `plugins\` holds an `.asi`; nothing else needs editing. The stock bat sets that key only when it downloads OptiPatcher, and the plugin can't set it itself: plugins reach the plugin API only through `LoadAsiPlugins`. `Remove_OptiScaler.bat` from the same setup deletes `OptiScaler\plugins\*`, the plugin and its profiles included.

Without a `[Plugin.lmxxf_nr]` section every key comes from the game's profile (`lmxxf_nr_profiles.ini` beside the plugin, matched on the exe name, see `docs/GAME_PROFILES.md`) and then from the built-in defaults. Any key set in `[Plugin.lmxxf_nr]` wins; `auto` or empty counts as unset. A profile's `Preset` is skipped when the user set Model, NetworkSize, MultiPass or Temporal. The tab shows `Profile: <Name>, sets <keys>`.

Anti-cheat: at start the plugin looks in the exe folder and up to three parents (and their `Plugins\`) for EasyAntiCheat, BattlEye, ACE, BlackCipher, EQU8, GameGuard, XIGNCODE, mhyprot, PunkBuster, FACEIT, EA Javelin and Denuvo Anti-Cheat files. It also checks a short list of exe names (Vanguard and VAC titles, Fortnite, Apex, R6, PUBG) and the loaded EAC / BattlEye client modules. On a hit it registers nothing but its tab and an overlay line saying it is disabled and why: no hooks, no callbacks, no adapter override.

Nothing of the runtime ships with the plugin. It is searched for in this order, and the first candidate whose dll, assets and module hashes check out is used:

1. `RuntimeDir` (with `ModulesDir` / `WeightsDir`) in `[Plugin.lmxxf_nr]`
2. `LMXXF_NR_DIR` (environment)
3. the plugin's folder, then `plugins\lmxxf_nr\`
4. `%LOCALAPPDATA%\lmxxf-nr\`
5. the game's exe folder and the OptiScaler folder (with `ModulesDir` / `WeightsDir` when no `RuntimeDir` is set)

The folders in 2-4 may hold a `paths.ini` (`[lmxxf-nr]` with `RuntimeDir`, `ModulesDir`, `WeightsDir`, relative to that folder) for a runtime that lives elsewhere; it is tried before the folder itself. In each candidate folder and its `DLSS5-AMD\`, the plugin looks for `LmxxfNrRuntime.dll` and its assets (below). Every module a `SHA256SUMS` lists must exist and hash right (80 modules: 0.9 s on first read, 50 ms cached), else that candidate is skipped and the reason logged. When nothing is found, the tab lists what was searched and where to put the runtime, and the overlay says `NR: off: no lmxxf runtime found`. The assets:

- **A runtime build with its module folder** (first when `ModulesDir` is set): `ModulesDir` (a HIP module folder with `SHA256SUMS`), else `modules\` or `HIP\` beside the dll, is the runtime's `assets_directory`; `WeightsDir` points the runtime at the tiled weights (`LMXXF_WEIGHTS_DIR`) when they are not beside the modules. Nothing is copied.
- **lmxxf's own package layout**: `DLSS5-AMD\native-game-tiled-assets\HIP\SHA256SUMS` + the HIP modules, weights in `native-game-tiled-assets\`. The runtime gets the HIP folder as `assets_directory`.
- **A runtime + `LmxxfNrRuntime.pak` beside it** (compatibility): for a repackaged runtime that keeps lmxxf's ABI v1 prefix. It gets only lmxxf's v1 functions and the 64-byte frame info every lmxxf-derived runtime reads the same way; a larger function table is accepted by size probing and only its v1 prefix is used.

`DLSS5_*` settings the runtime knows (found by name in the dll) are set in the process environment before the session is created; the menu greys out what the runtime does not read. `MultiPass` 2 / 3 ("Passes" in the menu, Full model only) runs the network two or three times per frame for a stronger style: the session is rebuilt off the render thread, without `FULL_NETWORK` (which pins the runtime to one pass). With 2 passes the history stays on when the runtime takes it with more than one pass (the temporal frame info's `passes`; lmxxf-nr `temporal` from 3ee91db6: the warped previous final answer goes into the last pass and the history gate blends the final output); an older runtime refuses it and the session runs without (`runtime takes no motion-fed history with 2 passes`). 3 passes run without history (the runtime takes it only with `DLSS5_MULTI_PASS_PREDICT=0`, at +50 % network time). The runtime keeps the pass count of the last `FULL_NETWORK` frame process-wide, so after a Full session the warm-up runs one `FULL_NETWORK` job before the real one to release it (also for Small). Network time on the temporal runtime, 1080 tier (`nr_sequence --perf`): 1 pass 10.4 ms, 2 passes 22.3 ms, 3 passes 17.7 ms (its default `DLSS5_MULTI_PASS_PREDICT=1` predicts the third pass). A runtime with lmxxf's history ABI (sole export, `GetTimings`, `history_supported = 1`, the `temporal` branch of lmxxf) gets the 168-byte temporal frame info, `FULL_NETWORK` for the Full model, and its history network built during the warm-up.

Keys (all optional; by hand without the setup change: `LoadAsiPlugins=true`):

```ini
[Plugins]
LoadAsiPlugins=true

[Plugin.lmxxf_nr]
Preset=0           ; 0 Custom, 1 Performance, 2 Balanced, 3 Quality, 4 Ultra: sets the next four rows at start
Placement=1        ; 0 before upscaling, 1 after upscaling, 2 terminal (HUD-less, at present)
NetworkSize=0      ; 0 1080 tier, 1 2560x1440, 2 native (1 and 2 need DLSS5_NETWORK_FREE_RES)
Model=0            ; 0 Full (71 blocks), 1 Small (31 blocks), DLSS5_MULTI_PASS 1 / 0.5
MultiPass=1        ; Full only: 1-3 network passes per frame (DLSS5_MULTI_PASS), 3 without history
Style=0            ; DLSS5_STYLE, 0 = NVIDIA's look; option -1 auto: 2 before / after upscaling, 0 terminal and multi-GPU
Strength=1.0
ColorStrength=1.0  ; share of the network's colour change: 1 as it is (faithful), 0 its luma on the game's chroma; option -1 auto: 0 before / after upscaling, 1 terminal and multi-GPU
EditUpsample=1     ; 1080 / 1440 sizes, edit to the output: 1 guided by the output-size colour (ratio, joint bilateral 3x3), 0 bilinear + feed residual
EditStabilise=true ; guided upsample: edit ratio accumulated at the output size (motion vectors, guide-change rejection, 3x3 spread clamp)
Temporal=true      ; motion-fed network history, when the runtime takes it
MultiGpu=false     ; see below, needs a restart
RuntimeDir=        ; folder of LmxxfNrRuntime.dll, tried first
ModulesDir=        ; HIP module folder (SHA256SUMS) of a runtime build
WeightsDir=        ; tiled weights of a runtime build
```

Presets (the first row of the menu) set only Model, Network size, Passes and history; every other row is left alone, and editing one of those four shows Custom. `Preset` is written on Save and, when not 0, overrides those four keys at start. A preset the runtime can't run (no `DLSS5_NETWORK_FREE_RES`, or one model only) is greyed out, its reason in the tooltip. With multi-GPU the display card's busy % is shown beside them as a headroom hint (display only). Network time on one card at 4K output, terminal placement:

| Preset | Model | Network size | Passes | History | Network (one card) |
|---|---|---|---|---|---|
| Performance | Small | 1080 tier | 1 | on | about 8 ms (55 fps, 76 without NR) |
| Balanced | Full | 1080 tier | 1 | on | about 18 ms (48 fps) |
| Quality | Full | 1440 | 1 | on | about 25 ms (35 fps) |
| Ultra | Full | native | 1 | on | about 55 ms (estimated, 2.25x the pixels of 1440 at 4K) |

With multi-GPU the display card runs the network beside the game (1080 tier about 8 ms there), so the cost stays hidden while that card has headroom; Ultra at 4K (about 25 ms there, estimated) likely does not fit a 60 fps frame. Passes 2 / 3 are a stronger style, not a higher quality step, so no preset uses them.

Multi-GPU, Satisfactory menu at 4K, NR every frame, preset switched live (`lmxxf_nr_settings.trigger`), means of 10 s windows: Quality 41.0 fps (24.4 ms, display card 57 % busy, network 13.6 ms); Ultra 21.9 fps (45.8 ms, display card 73 % busy, network 33.3 ms: the presenter waits for the answer each frame, so the network time is in the frame).

Motion vectors for the history are the game's, from the upscale callbacks, at the size the game gives them (render or output size; the runtime samples them over the feed's UV range, nearest texel): before and after upscaling straight from the evaluate, terminal from a copy taken in the evaluate of the same frame and used at its present, multi-GPU over the ring. History is reset on the game's reset flag, after a frame that was not fed, and when the vector extent or the network size changes.

## How it runs

- **Before / after upscaling** (upscale callbacks): the source (the render-size colour, or the upscaler's output) is box-filtered into the network size and fed (`PrepareFrame`, `RecordInputs`) on the game's own command list. When that list is submitted (an `ExecuteCommandLists` detour on the game's queue), the plugin's session queue waits for it and runs `EnqueueHip`; the game queue waits for the network only before the next frame's list that consumes it. The network's edit (its output minus what it was fed) is added to the next frame, moved by that frame's motion vectors; before upscaling the result is handed to the upscaler as its colour (`colorOverride`), after upscaling it is written into the output.
- **Terminal** (present callback, HUD-less request): NR on the frame's HUD-less colour (OptiScaler's HUDfix pick, or the game's FSR FG `HUDLessColor` while its FG is off), linearised from the swapchain's encoding (sRGB, PQ or linear); the edit is added to the back buffer where it matches the HUD-less colour and the UI pixels stay exactly as drawn. Runs on the plugin's own list on the present queue, before OptiScaler's menu and overlay. With the game's own FSR FG on, terminal runs at the game's Present on its FG swapchain (host detour, see docs/PLUGIN_API_EXTENSIONS.md): HUD-less NR on the real frame there, and the same edit written into the game's `HUDLessColor`, so the frames FFX generates are interpolated from NR'd frames. `WholeFrameFallback` (true): with no HUD-less colour for 30 presents in a row, or with the game's own frame generation on and no FG swapchain to run at (DLSS-G; every present, generated and real alike), NR runs on the whole frame, UI included; the menu's `Source:` line says which. Off: terminal waits for a HUD-less colour. A switch between HUD-less and whole frame resets the network history and the edit stabiliser (the feed gains or loses the UI). `TerminalHudless` (true; test rigs, live through `lmxxf_nr_ini.trigger`): false ignores the HUD-less colour, so the fallback takes over after 30 presents. Multi-GPU HUD-less mode takes the same key.
- **HDR before / after upscaling** (`IsHdr`): the colour is scene-linear (CONTROL: median about 1800, peaks 60000). A one-group stats pass maps it with the game's exposure texture / pre-exposure (`Exposure` off or no texture: a smoothed log-average auto-exposure, key 0.18), with the terminal HDR shoulder above 0.8; the edit goes back in scene units (additive below the knee, the same relative change above it), the feed is clamped to [0, 1] in every mode. Guard: when the mapped median is above 1.5, Apply leaves the frame alone and the menu / overlay say NR is bypassed (seen for a few frames at CONTROL's loading transitions). CONTROL dump (after upscaling): feed 0.002-0.994, median 0.215. The mapping is not what drains colour: the edit-back reproduces the network's change on the feed (saturation -13.1 % in scene units against -14.1 % fed -> network output), and feeding a tone-mapped picture instead (ACES, Hable, luminance Reinhard, a curve graded like the game's own output, exposures 0.15-1x) gives the same -18..-25 % in the scene crop with style 0. Style 0 on this scene colour desaturates and brightens (median +13 %), which is the network's look (DLSS 5 desaturates and adds contrast), so the defaults stay faithful. Options: `ColorStrength` 0 keeps the game's chroma with the network's luma (saturation +0.0 %), `Style` 2 has a third of style 0's tone shift before / after upscaling (median +3.6 %, mean -0.4 %) for as much detail. At the display-referred placements the network's change matches SDR: Satisfactory terminal, scene crop, PQ saturation -1.7 % against SDR -3.0 %.
- **Multi-GPU** (`MultiGpu=true`): the game's device goes to the card without monitors (adapter override), the swapchain is a proxy whose buffers live there and whose real swapchain presents on the display card (swapchain provider); each frame (and its HUD-less capture and motion vectors) goes over a cross-adapter ring and the display card runs NR, composites the UI back and presents (flip, tearing). The `mgpu/` modules are the ones of the fork's multi-GPU branch, made host-independent. Keys: `MgpuHudless` (true), `MgpuNrEveryFrame` (true: the display card waits for this frame's answer), `MgpuMotionVectors` (true), `MgpuUiGate` (0.90: a HUD-less / back buffer pair differing on more than this share of pixels is taken as a wrong pairing and presented without NR, never NR over the UI), `MgpuMaxFramesAhead` (1), `MgpuLowLatency` (2: the proxy Present waits for the render card's previous frame and paces the next one, about half the latency when the render card is the bottleneck for about 1 % fps; 1: the wait only; 0: off), `MgpuEarlySend` (false: lost the display device once in CONTROL with HUD-less and vectors), `MgpuOverlap` (0: the serial presenter; 1: the display card works on three threads and three queues, so a frame's ingest and the previous frame's present overlap the network, and the game sends the next frame just in time (see Overlap presenter)), `MgpuPresentIdle` (false: opt-in safety, the real Present goes only to a display queue that drained within 1 s, else the frame is not presented; about 1 ms per frame), `MgpuRenderBus` / `MgpuDisplayBus` (-1 = auto), `MgpuPresenter`, `MgpuFallbackInline`, `MgpuDred` (false: DRED on the display device, logs the removed reason, page fault and unfinished lists at the first failed present; `2`: page faults only, breadcrumbs serialise the queue and hide races), `MgpuForwardOutputs` (true: the render card's `EnumOutputs` returns the display card's outputs, so the game sees the real monitor, its HDR state and modes). `HdrPaperWhite` (200 nits) is the level the network's 1.0 maps to on an HDR output (see Robustness).

Menu: a "Neural Rendering" tab (settings, runtime, multi-GPU and NR stats, the colour mode NR runs in, the display output). FPS overlay (Detailed and fuller types): `NR: <network size> <model>[ T][ HDR], <GPU ms>` and, with multi-GPU, `MGPU: <load per card>, <added latency>, <NR ms>`; after a display card loss `NR: off (display card lost)` and `MGPU: display card lost: single-GPU present, NR off`.

Flicker meter (`FlickerMeter`, true): `tools/seq_metrics.py`'s flk_static and flk_mc computed on the GPU for every network answer in the multi-GPU and terminal placements (one dispatch at the feed size plus a copy of the pair, group sums read back a few frames later): how much more the answer changes from frame to frame than its input, on still pixels and on visible moving pixels warped with the vectors, in display encoding. Above 0 NR adds flicker, below 0 it steadies the image. Moving needs this frame's answer (multi-GPU with NR every frame). Menu: `Flicker: still ..., moving ...` (mean of the last 60 answers), overlay `flk` (moving, else still), log every 180 jobs. Against `seq_metrics.py` on 12-frame dumps of the same runs (Satisfactory menu, multi-GPU): Small 1080 with history -0.00075 / -0.00138 live against -0.00086 / -0.00131 offline (still / moving), without history +0.00066 / +0.00095 against +0.00057 / +0.00072, Full 1080 2 passes +0.00274 / +0.0034 against +0.00270 / +0.00338.

Log: `pacing:` every 10 s in every mode (presents, mean / median / p99 / max present-to-present, size, format, colour space).

Dumps: create `lmxxf_nr_dumps\lmxxf_nr_dump.trigger` in the OptiScaler folder: the next NR frame writes the network output, what it was fed and the result (`.raw` + `.bmp`); `mgpu_dump.trigger` there dumps multi-GPU frames (also in the single-GPU fallback), `mgpu_remove.trigger` removes the display device (recovery test). `lmxxf_nr_seq.trigger` (a frame count inside, default 12) dumps that many consecutive jobs: input, vectors and answer per frame plus `seq.json`; `tools/seq_metrics.py` scores flicker and ghosting on them. A sequence starts only once the network has run 30 jobs since its last build and its last history reset, starts over when either happens inside it, and while it waits the log says why (`sequence dump pending: ...`: rebuilding, NR held, window minimised, display card lost). `mgpu_minimize.trigger` (seconds inside, default 4) minimises the game's window without activating anything and restores it; `lmxxf_nr_settings.trigger` (`Key=Value` lines: `Preset`, `Model`, `NetworkSize`, `MultiPass`, `Temporal`, `Style`, `FlickerMeter`) applies settings live like the menu.

## Measured (Satisfactory main menu, 3840x2160, FSR 4 output, RDNA 4, the runtime + pak pair)

| Placement | fps | Network + handoff (GPU) | Edit (dump) |
|---|---|---|---|
| NR off | 73.3 | - | - |
| After upscaling, 1080 tier, overlapped | 40.2 | 18.6 ms (shares the GPU with the next frame) | mean 0.069, 98.8 % of pixels |
| After upscaling, `Serialize=true` (game waits for the network) | 35.5 | 9.8 ms (`hip_ms` 9.5-9.7) | mean 0.065 |
| Before upscaling (2304x1296 colour -> 1920x1080) | 27.2 | 15.8 ms | mean 0.069, 98.1 % |
| Terminal (HUD-less, at present) | 45.6 | 20.5 ms | UI pixels (26.3 %) identical to the back buffer, every scene pixel changed |
| Multi-GPU, whole frame, NR every frame | 63 (render card bound) | 9.6 ms on the display card | 99.9 % of pixels changed |
| Multi-GPU, HUD-less | 56 (presenter) | 9.7 ms | UI identical, scene changed |

Details: `docs/MGPU_DESIGN.md` section 11.

### lmxxf build (`temporal` branch, fenced modules, 0.45 weights), same scene

Uncapped menu, FSR 4 output 3840x2160, terminal placement (HUD-less) on one card or multi-GPU HUD-less with NR every frame. fps from PresentMon over 30 s; `hip` = network + handoff on the session queue, `net` = the runtime's own network time (`GetTimings`), medians. Every run presents with sync interval 0 and `ALLOW_TEARING`.

| Case | fps | p99 ms | hip ms | net ms |
|---|---|---|---|---|
| NR off | 76.1 | 14.5 | - | - |
| Terminal, Small 1080, history off | 59.3 / 59.6 | 30.7 | 8.1 | 6.2 |
| Terminal, Small 1080, history on | 54.6 / 54.7 | 33.6 | 8.8 | 7.8 |
| Terminal, Full 1080, history off | 51.8 | 20.8 | 17.1 | 16.9 |
| Terminal, Full 1080, history on | 48.3 | 21.9 | 18.7 | 18.4 |
| Terminal, Small native 2560x1440, history on | 45.7 | 23.9 | 17.5 | 16.3 |
| Multi-GPU, NR off | 56.9 | - | - | - |
| Multi-GPU, Small 1080, history off | 56.7 | 20.4 | 6.4 | 6.2 |
| Multi-GPU, Small 1080, history on | 56.0 | 19.1 | 7.8 | 7.6 |
| Multi-GPU, Full 1080, history on | 56.2 | 19.4 | 8.4 | 8.2 |
| Multi-GPU, Small native 2560x1440, history on | 56.8 | 19.1 | 8.2 | 8.0 |

Multi-GPU is render card bound (93 % busy) with NR in every case; the display card is 37-46 % busy with NR, 18 % without. With NR every frame the presenter holds the real present until the frame's network answer is done: a present queued on the display queue behind the still unresolved wait on the HIP answer page-faulted the display device (`DXGI_ERROR_DEVICE_HUNG`, DRED device state page fault, at the present) within a second of the first network frame. DRED breadcrumbs hid it by serialising the queue, and AMDNR's pair never hit it. The rows above were measured with `MgpuDred=true` before that fix; without DRED the fixed build runs about 0.4 ms per frame slower (render 18.3-18.4 ms against 17.7-18.0).

History on game frames (`tools/seq_metrics.py`, 7 sequences of 12 frames per run, Small 1080; the menu camera is mostly still with occasional pans):

| Run | flk_static | flk_mc | psnr_mc | edit_dis |
|---|---|---|---|---|
| Terminal, history off | +0.00065 | +0.00084 | 38.7 | 0.0277 |
| Terminal, history on | -0.00055 | -0.00061 | 41.3 | 0.0223 |
| Multi-GPU, history off | +0.00065 | +0.00099 | 38.3 | 0.0263 |
| Multi-GPU, history on (6 sequences, a cut excluded) | -0.00056 | -0.00056 | 40.5 | 0.0224 |

Full 1080 tier, multi-GPU, same menu, 12-frame dumps (`seq_metrics.py`, flk_static / flk_mc; the live meter in brackets): 1 pass with history -0.00062 / -0.00012 (-0.0006 / -0.0002 to -0.0014), 2 passes without history +0.00277 / +0.00293 (+0.0027 / +0.0019 to +0.0021), 2 passes with history (lmxxf-nr `temporal` 3ee91db6) +0.00052 / +0.00100 (+0.0006 / +0.0015 to +0.0024), edit_dis 0.036 / 0.077 / 0.057; network 12.2 ms (1 pass, history) and 16.5 ms (2 passes, history). CONTROL gameplay, Full 1080 2 passes without history (the play session): +0.0018 / +0.0055 once the pair spanning a network rebuild (+0.035) is left out.

Without history the output changes more than its input from frame to frame (positive flicker); with history it changes less, on still and on motion-compensated pixels, and the edit on disoccluded pixels does not grow (no ghosting signal). Panning sequences (1-5 px/frame, 6-20 % of pixels moving): flk_mc +0.00095 / psnr_mc 35.8 off against -0.00026..-0.00060 / 37.5-44.1 on. The vectors' sign checks out on every sequence (warping with +mv beats -mv and no motion).

### Overlap presenter (`MgpuOverlap`)

**Dropped (2026-11-11), this branch is an archive.** With the network asynchronous (`NrCadence=async`, spike/nr-async) the presenter never waits for it, and overlap adds nothing on top: Quality 64.88 against 65.14 fps, Ultra 61.08 against 61.83, latency 14.7-14.9 ms in all four (Satisfactory, cold, `MgpuDred=1`). Async alone gives more than overlap with every-frame NR (Quality 45.4 to 64.9 fps at 14.7 ms, against overlap's 53.1 fps at 27.5 ms). The PCIe link line and hints, the added-latency stats, the device-create race fix and the harness options went to spike/nr-async on their own.

Satisfactory main menu, 3840x2160, FSR 4 output, HUD-less, temporal; one cold launch per preset with a 20 s overlap window, then a 20 s serial window (the presenter switched live). Both links at 28 GB/s (bw.exe). `MgpuDred=1` in all four: DRED breadcrumbs on the display device were the interim mitigation for the display-card losses at that time. Latency is the game's Present to the real Present.

| Preset | serial fps | overlap fps | serial latency ms | overlap latency ms |
|---|---|---|---|---|
| Performance | 67.26 | 64.15 | 14.8 | 16.8 |
| Balanced | 63.80 | 64.08 | 15.6 | 19.0 |
| Quality | 45.37 | 53.12 (+17 %) | 22.0 | 27.5 |
| Ultra | 22.73 | 24.11 (+6 %) | 44.0 | 51.6 |

Satisfactory's render card needs about 14.9 ms per frame. At Performance and Balanced it is the bound, and overlap gains nothing: at Performance it loses 3 fps, because with the grant held back each frame runs its three stages nearly one after another. At Quality and Ultra the display card is the bound, and overlap gains what ingest and present no longer add to the network time.

The game may send the next frame early ("just in time") only while the display card's work stage is about to be free and the render card has already finished the frame being presented. Without that last condition, a render-bound game queued a second frame on the render card: same frame rate, 11 ms more latency. A variant that let the game run 2 frames ahead was removed: same frame rates, a whole display frame more latency.

Harness (`tools/mgpu_harness`, no render load, so the display card is the bound): Performance 116.5 to 138.0 fps, Balanced 79.9 to 91.3, Quality 52.9 to 56.1, Ultra 24.9 to 25.4; with a 20 ms render load (render-bound) 61.4 to 68.6 fps and 16.3 to 14.6 ms. Render-queue stalls of 2.5 s and 3.5 s, live presenter switches, and the upscaler-start / HUD-less gate sequence (fake upscaler, `--upscaler --hudless --upscaler-late --script`) pass.

## Robustness

Checked on the Satisfactory main menu (3840x2160, FSR 4, Small 1080 tier with history) with in-process logs and frame dumps only; the rig scripts live in the git-ignored `data/`.

- **Long runs**: 10 minutes each, multi-GPU HUD-less and single-GPU terminal: no failed present or device removal, process memory and dedicated / shared VRAM of both cards flat after the first 2 minutes (all within 0.6 MB/min), frame time drift -0.3 % (multi-GPU, 18.4 ms) and -0.9 % (terminal, 18.3 ms), p99 under 21 ms in every 10 s window after the warm-up. Repeated on the final build of this pass: multi-GPU drift -0.1 %, VRAM of both cards exactly flat; terminal drift +1.6 %, memory flat, one 40 s stretch with frames up to 5.2 s while NR's own GPU time stayed at 8 ms (no plugin log line; cause not found).
- **Resize** (`ResizeBuffers`, `ResizeBuffers1`, `ResizeTarget`): the proxy stops the presenter, waits for the network's queued work, drops frames sent but not presented, resizes the real swapchain and rebuilds the ring, the HUD-less and motion staging and the UI composite at the new size and format. The previous size's resources are kept until 30 more frames were presented (`ReleaseBuffers(..., retire)`). The network history is reset whenever the source extent changes, in every placement (`NR (...): source AxB -> CxD, history reset`). Tested with the game's own resize at start-up (windowed 1920x1080 -> its saved 3840x2160) and window resizes 3840x2160 -> 2400x1350 -> 3840x2160 (SetWindowPos, no input): single-GPU terminal keeps NR through all of them.
- **Fullscreen**: `SetFullscreenState` and `ResizeTarget` go to the display card's real swapchain with the presenter stopped (DXGI sends mode changes to the window thread while a Present may wait for it: holding the swapchain mutex across the real Present deadlocked the first exclusive-fullscreen run), `GetFullscreenState` answers from the state the presenter keeps current, `GetContainingOutput` is the display card's output. Satisfactory's fullscreen mode never asks for exclusive fullscreen (no `SetFullscreenState(TRUE)`, it is a flip swapchain on a monitor-sized window). `tools/mgpu_harness --fse` does: enter and leave return S_OK and its `ResizeBuffers` pass; Windows does not keep exclusive mode for a window that is not in front (the rig never takes focus), and the real swapchain then refuses presents until it is resized, which the proxy does itself (`real swapchain resized after a refused present`), so the game never sees a failed present.
- **Resize while NR runs on the display card**: the game resizes its window before it calls `ResizeBuffers`; NR frames presented while the swapchain is smaller or larger than its window removed the display device (`DEVICE_HUNG`, a page fault at an address outside every live or recently freed D3D12 allocation per DRED, hidden by DRED breadcrumbs, never with NR off, also with `MgpuNrEveryFrame=false`). NR is now held while the client size differs from the swapchain, for at most 5 s (a swapchain that stays smaller than its window, stretch scaling, gets NR back): 6 of 6 resize runs lost the device before, 8 of 8 resizes in 2 runs pass with the hold (`window AxB, swapchain CxD: NR held / resumed`).
- **HDR**: the render card has no outputs, so the game's `EnumOutputs` on it returns the display card's (`MgpuForwardOutputs`), and `GetContainingOutput`, `CheckColorSpaceSupport`, `SetColorSpace1` and `SetHDRMetaData` go to the display card's swapchain. Satisfactory with `r.HDR.EnableHDROutput=1` (Engine.ini, the menu option alone does not switch it): `CheckColorSpaceSupport(ST 2084)` supported, `SetColorSpace1(12)` accepted, PQ (`R10G10B10A2`, colour space 12) for the whole run, multi-GPU 7.8 ms and terminal 8.9 ms of network + handoff as in SDR. The game picks PQ also for the scRGB output device; FP16 is checked with the harness (`--format fp16`, scRGB, bars up to 320 nits): ring, NR and present at 16.5 ms median, the result peaks at 335 nits, pixels above 200 nits change by 7.6 % relative, no negative output. The network was trained on SDR-like input, so on an HDR output it is fed paper-white-relative linear values (`HdrPaperWhite`, 200 nits = 1.0) with a C1 Reinhard shoulder above 0.8, and its edit is added back additively in paper-white units below the knee and as the same relative change above it. Measured on the dumps (930-nit peak menu): the result peaks at 921-964 nits, pixels above 400 nits change by 4-4.5 % relative, the mean change is 2.5 nits on an 18-nit mean; the feed stays at or below 0.99. Adding the edit through the shoulder's inverse instead took highlights to 10000 nits. Menu: `Colour: HDR10 (PQ), the network gets tonemapped values`; overlay `NR: ... HDR`.
- **Minimise / exclusive fullscreen left by DXGI with NR running** (CONTROL, Full 1440, 2 passes): DXGI leaves exclusive fullscreen when the window is minimised, the real Present returns `DXGI_STATUS_OCCLUDED`, the next one `INVALID_CALL`, the presenter resizes the real swapchain, and the next `Present(1, 0)` on the minimised window blocked 4 s and returned `DEVICE_REMOVED` (`DEVICE_HUNG`); the display card's 3D queue showed busy over 100 % for the 2 s before it, the render card idle. The presenter now presents nothing while the window is minimised or its last present said occluded: each frame only releases its ring slot, a `DXGI_PRESENT_TEST` present tells when the window shows again, and the proxy answers the game's test presents with occluded meanwhile. `mgpu_minimize.trigger` every 11 s, 5 cycles per run: before, the first cycle in exclusive fullscreen lost the display card (as in the play session); with it 10 of 10 cycles pass, 2 of them leaving exclusive fullscreen. The window-size hold below stays for real resizes.
- **Display card loss**: when the display device is removed (a failed present, the removed reason checked every 30 frames, or found removed at a resize) the presenter stops and the next game present moves to a swapchain on the render card's queue (Windows' cross-adapter present) that presents copies of the game's own back buffers, no NR, and keeps following `ResizeBuffers`. Overlay: `MGPU: display card lost: single-GPU present, NR off`. `mgpu_remove.trigger` in the dump folder removes the display device (`ID3D12Device5::RemoveDevice`) to test it: one 201 ms frame at the switch, then 15.4 ms per frame from the render card. In exclusive fullscreen the real swapchain leaves fullscreen first (releasing a fullscreen swapchain raised a DXGI exception and crashed CONTROL) and the fallback swapchain is windowed; a fallback present that says occluded while the window shows is returned as `S_OK` (games throttle on it). CONTROL menu, Full 1440 x2, display device removed in exclusive fullscreen: fallback at 6.2 ms per frame (the game's rate there), through a minimise as well. Each rebuild of the model pin (the `FULL_NETWORK` warm-up before a multi-pass session) now runs once per change from Full 1 pass, not before every rebuild.

`tools/mgpu_harness` (`build.ps1`): a minimal D3D12 title (borderless window on the primary monitor, moving bars, a frame counter strip, tearing present) to run the redirect, the proxy and NR without a game: put OptiScaler as `dxgi.dll` with the plugin beside it. `--format sdr|pq|fp16`, `--fse <s>` (exclusive fullscreen for 4 s), `--resize <s>` (half size for 4 s); it prints every swapchain call's result and reads no input.

### Validation runs and the lifetime fuzzer

Three display card losses had one cause: a resource freed or replaced while submitted work still used it, each after a live setting switch. `tools/lifetime_fuzz` looks for that class without waiting for the device to hang.

- **`Validation`** (`[Plugin.lmxxf_nr]`, 0; read at start): 1 turns on the D3D12 debug layer and DRED (breadcrumbs and page faults) for every device of the process, the game's and the display card's, from `OptiPluginInit` (the debug layer has to be on before the first device exists); 2 adds GPU-based validation. Each device's messages go to the OptiScaler log as `d3d12 <game|render|display> #<id> <severity> x<count>: <text>` (the first 8 of an id per device, then every power of two), and a removed device gets its reason, DRED page fault and unfinished lists logged once. Every resource, list and allocator the plugin creates has a name, so `#921 OBJECT_DELETED_WHILE_STILL_IN_USE` and DRED say which object and which queue. It needs Windows' Graphics Tools feature (`d3d12SDKLayers.dll`). Level 1 leaves a network rebuild at its usual 2-3 s; level 2 takes 5-11 s for one.
- **Test hooks** (`TestHooks=true`): `LmxxfNrTestUpscale(frame, after)` calls the plugin's upscale callbacks, `LmxxfNrTestHudless(resource, state)` gives the HUD-less colour of the frame presented next, `LmxxfNrTestStatus()` says whether a device was lost or NR failed. `TriggerEvery` (30) is the number of presents between looks at the trigger files.
- **`tools/mgpu_harness --upscaler --hudless`**: the bars are rendered at `--render-scale` with motion vectors, the plugin's callbacks run around the harness's own scaling pass, and the picture before the counter strip is the HUD-less colour: before / after upscaling, terminal HUD-less and multi-GPU HUD-less run without a game.
- **`tools/mgpu_harness --fuzz <seed>`**: a seeded sequence of live switches, each printed with its frame: `lmxxf_nr_ini.trigger` keys (Preset, Model, NetworkSize, MultiPass, Temporal, Placement, TerminalHudless, EditUpsample, EditStabilise, Enabled, MotionVectors, FlickerMeter, WholeFrameFallback, Style, Serialize), frame and sequence dumps, the multi-GPU triggers, resizes with the window ahead of `ResizeBuffers`, minimise, the HUD-less source going away and coming back, render scale changes, the reset flag and frames without vectors. Steps are keyed on the frame count, so a seed replays; `--fuzz-classes` keeps the sequence and applies only the named classes, `--fuzz-steps` cuts it short. The run ends at a device loss (exit 5) or a failed NR (exit 6).
- **`tools/lifetime_fuzz/run.py`** stages OptiScaler, the plugin and the harness, runs one fuzz run per mode (`single`, `mgpu`: the multi-GPU start keys also follow from the seed) and seed, and merges the debug layer messages, plugin errors and device losses into `findings.md`, each with the switches that came before it.

Open: holding exclusive fullscreen for real needs the game in front; resizes now hold NR for the frames between the window's and the swapchain's resize, the driver-side reason those frames removed the device is not known. The single-GPU fallback ran at 10 fps in a CONTROL gameplay session with the game's FSR frame generation on (render card mostly idle, so a wait, not the copy); the menu without frame generation runs at the game's rate, so that case is not reproduced. The display device is not recreated after a loss: the game stays on the fallback until it restarts.

## Build

`build.ps1` (VS 2022, v143, Release x64, logical threads - 2) builds `x64\lmxxf_nr\lmxxf_nr.asi` at the repository root. It uses OptiScaler's plugin SDK header (`src/plugin_sdk/`), lmxxf's MIT header (`third_party/lmxxf/`) and Microsoft Detours (MIT) as vendored by OptiScaler under `src/`.

## Credits

lmxxf's DLSS 5 on AMD runtime and C API: Kien (MIT). OptiScaler: cdozdil and contributors.
