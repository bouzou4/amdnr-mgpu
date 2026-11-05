# lmxxf_nr: DLSS 5 Neural Rendering for OptiScaler, as a plugin

An OptiScaler ASI plugin that runs lmxxf's DLSS 5 Neural Rendering runtime (`LmxxfNrRuntime.dll`, lmxxf's MIT C API `LmxxfNrApi.h`) around the upscaler or at present, on one GPU or on a second one. MIT licence (`LICENSE`).

It needs an OptiScaler with the plugin API extensions of this repository (`docs/PLUGIN_API_EXTENSIONS.md`): upscale callbacks (1) for the before / after placements, overlay text (3), the present callback (2) for the terminal placement and the HUD-less capture, and the adapter override (5) + swapchain provider (6) for multi-GPU. Each feature checks its API field and stays off without it.

## What the user installs

Nothing of the runtime ships with the plugin. It looks for a user-installed `LmxxfNrRuntime.dll` in `RuntimeDir` (setting), the game's exe folder, the OptiScaler folder and their `DLSS5-AMD\` subfolders, and for its assets:

- **A runtime build with its module folder** (first when `ModulesDir` is set): `ModulesDir` (a HIP module folder with `SHA256SUMS`), else `modules\` or `HIP\` beside the dll, is the runtime's `assets_directory`; `WeightsDir` points the runtime at the tiled weights (`LMXXF_WEIGHTS_DIR`) when they are not beside the modules. Nothing is copied.
- **lmxxf's own package layout**: `DLSS5-AMD\native-game-tiled-assets\HIP\SHA256SUMS` + the HIP modules, weights in `native-game-tiled-assets\`. The runtime gets the HIP folder as `assets_directory`.
- **A runtime + `LmxxfNrRuntime.pak` beside it** (compatibility): for a repackaged runtime that keeps lmxxf's ABI v1 prefix. It gets only lmxxf's v1 functions and the 64-byte frame info every lmxxf-derived runtime reads the same way; a larger function table is accepted by size probing and only its v1 prefix is used.

`DLSS5_*` settings the runtime knows (found by name in the dll) are set in the process environment before the session is created; the menu greys out what the runtime does not read. `MultiPass` 2 / 3 ("Passes" in the menu, Full model only) runs the network two or three times per frame for a stronger style: the session is rebuilt off the render thread, without `FULL_NETWORK` (which pins the runtime to one pass) and without history (the runtime's history takes one pass). The runtime keeps the pass count of the last `FULL_NETWORK` frame process-wide, so after a Full session the warm-up runs one `FULL_NETWORK` job before the real one to release it (also for Small). Network time on the temporal runtime, 1080 tier (`nr_sequence --perf`): 1 pass 10.4 ms, 2 passes 22.3 ms, 3 passes 17.7 ms (its default `DLSS5_MULTI_PASS_PREDICT=1` predicts the third pass). A runtime with lmxxf's history ABI (sole export, `GetTimings`, `history_supported = 1`, the `temporal` branch of lmxxf) gets the 168-byte temporal frame info, `FULL_NETWORK` for the Full model, and its history network built during the warm-up.

Install: `<game>\OptiScaler\plugins\lmxxf_nr.asi` (OptiScaler's `MainDllPath` plugins folder) and in `OptiScaler.ini`:

```ini
[Plugins]
LoadAsiPlugins=true

[Plugin.lmxxf_nr]
Preset=0           ; 0 Custom, 1 Performance, 2 Balanced, 3 Quality, 4 Ultra: sets the next four rows at start
Placement=1        ; 0 before upscaling, 1 after upscaling, 2 terminal (HUD-less, at present)
NetworkSize=0      ; 0 1080 tier, 1 2560x1440, 2 native (1 and 2 need DLSS5_NETWORK_FREE_RES)
Model=0            ; 0 Full (71 blocks), 1 Small (31 blocks), DLSS5_MULTI_PASS 1 / 0.5
MultiPass=1        ; Full only: 1-3 network passes per frame (DLSS5_MULTI_PASS), 2 and 3 without history
Style=0            ; DLSS5_STYLE, 0 = NVIDIA's look
Strength=1.0
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

Motion vectors for the history are the game's, from the upscale callbacks, at the size the game gives them (render or output size; the runtime samples them over the feed's UV range, nearest texel): before and after upscaling straight from the evaluate, terminal from a copy taken in the evaluate of the same frame and used at its present, multi-GPU over the ring. History is reset on the game's reset flag, after a frame that was not fed, and when the vector extent or the network size changes.

## How it runs

- **Before / after upscaling** (upscale callbacks): the source (the render-size colour, or the upscaler's output) is box-filtered into the network size and fed (`PrepareFrame`, `RecordInputs`) on the game's own command list. When that list is submitted (an `ExecuteCommandLists` detour on the game's queue), the plugin's session queue waits for it and runs `EnqueueHip`; the game queue waits for the network only before the next frame's list that consumes it. The network's edit (its output minus what it was fed) is added to the next frame, moved by that frame's motion vectors; before upscaling the result is handed to the upscaler as its colour (`colorOverride`), after upscaling it is written into the output.
- **Terminal** (present callback, HUD-less request): NR on the frame's HUD-less colour (OptiScaler's HUDfix pick, or the game's FSR FG `HUDLessColor` while its FG is off), linearised from the swapchain's encoding (sRGB, PQ or linear); the edit is added to the back buffer where it matches the HUD-less colour and the UI pixels stay exactly as drawn. Runs on the plugin's own list on the present queue, before OptiScaler's menu and overlay. `WholeFrameFallback` (true): with no HUD-less colour for 30 presents in a row, or with the game's own frame generation on (every present, generated and real alike), NR runs on the whole frame, UI included; the menu's `Source:` line says which. Off: terminal waits for a HUD-less colour. Multi-GPU HUD-less mode takes the same key.
- **HDR before / after upscaling** (`IsHdr`): the colour is scene-linear (CONTROL: median about 1800, peaks 60000). A one-group stats pass maps it with the game's exposure texture / pre-exposure (`Exposure` off or no texture: a smoothed log-average auto-exposure, key 0.18), with the terminal HDR shoulder above 0.8; the edit goes back in scene units (additive below the knee, the same relative change above it), the feed is clamped to [0, 1] in every mode. Guard: when the mapped median is above 1.5, Apply leaves the frame alone and the menu / overlay say NR is bypassed (seen for a few frames at CONTROL's loading transitions). CONTROL dump (after upscaling, Small): feed 0.002-0.994, median 0.215; saturation 0.30 -> 0.26 (network output 0.32 -> 0.25 on the feed; Satisfactory PQ loses 0.39 -> 0.36), luma median +11 %.
- **Multi-GPU** (`MultiGpu=true`): the game's device goes to the card without monitors (adapter override), the swapchain is a proxy whose buffers live there and whose real swapchain presents on the display card (swapchain provider); each frame (and its HUD-less capture and motion vectors) goes over a cross-adapter ring and the display card runs NR, composites the UI back and presents (flip, tearing). The `mgpu/` modules are the ones of the fork's multi-GPU branch, made host-independent. Keys: `MgpuHudless` (true), `MgpuNrEveryFrame` (true: the display card waits for this frame's answer), `MgpuMotionVectors` (true), `MgpuUiGate` (0.90: a HUD-less / back buffer pair differing on more than this share of pixels is taken as a wrong pairing and presented without NR, never NR over the UI), `MgpuMaxFramesAhead` (1), `MgpuRenderBus` / `MgpuDisplayBus` (-1 = auto), `MgpuPresenter`, `MgpuFallbackInline`, `MgpuDred` (false: DRED on the display device, logs the removed reason, page fault and unfinished lists at the first failed present; `2`: page faults only, breadcrumbs serialise the queue and hide races), `MgpuForwardOutputs` (true: the render card's `EnumOutputs` returns the display card's outputs, so the game sees the real monitor, its HDR state and modes). `HdrPaperWhite` (200 nits) is the level the network's 1.0 maps to on an HDR output (see Robustness).

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

Without history the output changes more than its input from frame to frame (positive flicker); with history it changes less, on still and on motion-compensated pixels, and the edit on disoccluded pixels does not grow (no ghosting signal). Panning sequences (1-5 px/frame, 6-20 % of pixels moving): flk_mc +0.00095 / psnr_mc 35.8 off against -0.00026..-0.00060 / 37.5-44.1 on. The vectors' sign checks out on every sequence (warping with +mv beats -mv and no motion).

## Robustness

Checked on the Satisfactory main menu (3840x2160, FSR 4, Small 1080 tier with history) with in-process logs and frame dumps only; the rig scripts live in the git-ignored `data/`.

- **Long runs**: 10 minutes each, multi-GPU HUD-less and single-GPU terminal: no failed present or device removal, process memory and dedicated / shared VRAM of both cards flat after the first 2 minutes (all within 0.6 MB/min), frame time drift -0.3 % (multi-GPU, 18.4 ms) and -0.9 % (terminal, 18.3 ms), p99 under 21 ms in every 10 s window after the warm-up. Repeated on the final build of this pass: multi-GPU drift -0.1 %, VRAM of both cards exactly flat; terminal drift +1.6 %, memory flat, one 40 s stretch with frames up to 5.2 s while NR's own GPU time stayed at 8 ms (no plugin log line; cause not found).
- **Resize** (`ResizeBuffers`, `ResizeBuffers1`, `ResizeTarget`): the proxy stops the presenter, waits for the network's queued work, drops frames sent but not presented, resizes the real swapchain and rebuilds the ring, the HUD-less and motion staging and the UI composite at the new size and format. The previous size's resources are kept until 30 more frames were presented (`ReleaseBuffers(..., retire)`). The network history is reset whenever the source extent changes, in every placement (`NR (...): source AxB -> CxD, history reset`). Tested with the game's own resize at start-up (windowed 1920x1080 -> its saved 3840x2160) and window resizes 3840x2160 -> 2400x1350 -> 3840x2160 (SetWindowPos, no input): single-GPU terminal keeps NR through all of them.
- **Fullscreen**: `SetFullscreenState` and `ResizeTarget` go to the display card's real swapchain with the presenter stopped (DXGI sends mode changes to the window thread while a Present may wait for it: holding the swapchain mutex across the real Present deadlocked the first exclusive-fullscreen run), `GetFullscreenState` answers from the state the presenter keeps current, `GetContainingOutput` is the display card's output. Satisfactory's fullscreen mode never asks for exclusive fullscreen (no `SetFullscreenState(TRUE)`, it is a flip swapchain on a monitor-sized window). `tools/mgpu_harness --fse` does: enter and leave return S_OK and its `ResizeBuffers` pass; Windows does not keep exclusive mode for a window that is not in front (the rig never takes focus), and the real swapchain then refuses presents until it is resized, which the proxy does itself (`real swapchain resized after a refused present`), so the game never sees a failed present.
- **Resize while NR runs on the display card**: the game resizes its window before it calls `ResizeBuffers`; NR frames presented while the swapchain is smaller or larger than its window removed the display device (`DEVICE_HUNG`, a page fault at an address outside every live or recently freed D3D12 allocation per DRED, hidden by DRED breadcrumbs, never with NR off, also with `MgpuNrEveryFrame=false`). NR is now held while the client size differs from the swapchain, for at most 5 s (a swapchain that stays smaller than its window, stretch scaling, gets NR back): 6 of 6 resize runs lost the device before, 8 of 8 resizes in 2 runs pass with the hold (`window AxB, swapchain CxD: NR held / resumed`).
- **HDR**: the render card has no outputs, so the game's `EnumOutputs` on it returns the display card's (`MgpuForwardOutputs`), and `GetContainingOutput`, `CheckColorSpaceSupport`, `SetColorSpace1` and `SetHDRMetaData` go to the display card's swapchain. Satisfactory with `r.HDR.EnableHDROutput=1` (Engine.ini, the menu option alone does not switch it): `CheckColorSpaceSupport(ST 2084)` supported, `SetColorSpace1(12)` accepted, PQ (`R10G10B10A2`, colour space 12) for the whole run, multi-GPU 7.8 ms and terminal 8.9 ms of network + handoff as in SDR. The game picks PQ also for the scRGB output device; FP16 is checked with the harness (`--format fp16`, scRGB, bars up to 320 nits): ring, NR and present at 16.5 ms median, the result peaks at 335 nits, pixels above 200 nits change by 7.6 % relative, no negative output. The network was trained on SDR-like input, so on an HDR output it is fed paper-white-relative linear values (`HdrPaperWhite`, 200 nits = 1.0) with a C1 Reinhard shoulder above 0.8, and its edit is added back additively in paper-white units below the knee and as the same relative change above it. Measured on the dumps (930-nit peak menu): the result peaks at 921-964 nits, pixels above 400 nits change by 4-4.5 % relative, the mean change is 2.5 nits on an 18-nit mean; the feed stays at or below 0.99. Adding the edit through the shoulder's inverse instead took highlights to 10000 nits. Menu: `Colour: HDR10 (PQ), the network gets tonemapped values`; overlay `NR: ... HDR`.
- **Minimise / exclusive fullscreen left by DXGI with NR running** (CONTROL, Full 1440, 2 passes): DXGI leaves exclusive fullscreen when the window is minimised, the real Present returns `DXGI_STATUS_OCCLUDED`, the next one `INVALID_CALL`, the presenter resizes the real swapchain, and the next `Present(1, 0)` on the minimised window blocked 4 s and returned `DEVICE_REMOVED` (`DEVICE_HUNG`); the display card's 3D queue showed busy over 100 % for the 2 s before it, the render card idle. The presenter now presents nothing while the window is minimised or its last present said occluded: each frame only releases its ring slot, a `DXGI_PRESENT_TEST` present tells when the window shows again, and the proxy answers the game's test presents with occluded meanwhile. `mgpu_minimize.trigger` every 11 s, 5 cycles per run: before, the first cycle in exclusive fullscreen lost the display card (as in the play session); with it 10 of 10 cycles pass, 2 of them leaving exclusive fullscreen. The window-size hold below stays for real resizes.
- **Display card loss**: when the display device is removed (a failed present, the removed reason checked every 30 frames, or found removed at a resize) the presenter stops and the next game present moves to a swapchain on the render card's queue (Windows' cross-adapter present) that presents copies of the game's own back buffers, no NR, and keeps following `ResizeBuffers`. Overlay: `MGPU: display card lost: single-GPU present, NR off`. `mgpu_remove.trigger` in the dump folder removes the display device (`ID3D12Device5::RemoveDevice`) to test it: one 201 ms frame at the switch, then 15.4 ms per frame from the render card. In exclusive fullscreen the real swapchain leaves fullscreen first (releasing a fullscreen swapchain raised a DXGI exception and crashed CONTROL) and the fallback swapchain is windowed; a fallback present that says occluded while the window shows is returned as `S_OK` (games throttle on it). CONTROL menu, Full 1440 x2, display device removed in exclusive fullscreen: fallback at 6.2 ms per frame (the game's rate there), through a minimise as well. Each rebuild of the model pin (the `FULL_NETWORK` warm-up before a multi-pass session) now runs once per change from Full 1 pass, not before every rebuild.

- **Known issue, not ours: Satisfactory on Vulkan doesn't exit**, even without OptiScaler (no proxy DLL at all) and without the plugin. After the window closes, one thread is left in the game's own CRT exit table: `~FOutputDeviceFile` -> `~FAsyncWriter` -> `FAsyncWriter::Flush` spin-waits for the log-writer thread, which ExitProcess has already killed. Vanilla D3D12 exits normally (4 s after close). The process lingers, holding the game's DLLs, until it is terminated. The rig's watchdog terminates it about 20 s after close and then restores the game folder. Players have to end the process themselves.

`tools/mgpu_harness` (`build.ps1`): a minimal D3D12 title (borderless window on the primary monitor, moving bars, a frame counter strip, tearing present) to run the redirect, the proxy and NR without a game: put OptiScaler as `dxgi.dll` with the plugin beside it. `--format sdr|pq|fp16`, `--fse <s>` (exclusive fullscreen for 4 s), `--resize <s>` (half size for 4 s); it prints every swapchain call's result and reads no input.

Open: holding exclusive fullscreen for real needs the game in front; resizes now hold NR for the frames between the window's and the swapchain's resize, the driver-side reason those frames removed the device is not known. The single-GPU fallback ran at 10 fps in a CONTROL gameplay session with the game's FSR frame generation on (render card mostly idle, so a wait, not the copy); the menu without frame generation runs at the game's rate, so that case is not reproduced. The display device is not recreated after a loss: the game stays on the fallback until it restarts.

## Build

`build.ps1` (VS 2022, v143, Release x64, logical threads - 2) builds `x64\lmxxf_nr\lmxxf_nr.asi` at the repository root. It uses OptiScaler's plugin SDK header (`src/plugin_sdk/`), lmxxf's MIT header (`third_party/lmxxf/`) and Microsoft Detours (MIT) as vendored by OptiScaler under `src/`.

## Credits

lmxxf's DLSS 5 on AMD runtime and C API: Kien (MIT). OptiScaler: cdozdil and contributors.
