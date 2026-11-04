# lmxxf_nr: DLSS 5 Neural Rendering for OptiScaler, as a plugin

An OptiScaler ASI plugin that runs lmxxf's DLSS 5 Neural Rendering runtime (`LmxxfNrRuntime.dll`, lmxxf's MIT C API `LmxxfNrApi.h`) around the upscaler or at present, on one GPU or on a second one. MIT licence (`LICENSE`).

It needs an OptiScaler with the plugin API extensions of this repository (`docs/PLUGIN_API_EXTENSIONS.md`): upscale callbacks (1) for the before / after placements, overlay text (3), the present callback (2) for the terminal placement and the HUD-less capture, and the adapter override (5) + swapchain provider (6) for multi-GPU. Each feature checks its API field and stays off without it.

## What the user installs

Nothing of the runtime ships with the plugin. It looks for a user-installed `LmxxfNrRuntime.dll` in `RuntimeDir` (setting), the game's exe folder, the OptiScaler folder and their `DLSS5-AMD\` subfolders, and for its assets:

- **A runtime build with its module folder** (first when `ModulesDir` is set): `ModulesDir` (a HIP module folder with `SHA256SUMS`), else `modules\` or `HIP\` beside the dll, is the runtime's `assets_directory`; `WeightsDir` points the runtime at the tiled weights (`LMXXF_WEIGHTS_DIR`) when they are not beside the modules. Nothing is copied.
- **lmxxf's own package layout**: `DLSS5-AMD\native-game-tiled-assets\HIP\SHA256SUMS` + the HIP modules, weights in `native-game-tiled-assets\`. The runtime gets the HIP folder as `assets_directory`.
- **A runtime + `LmxxfNrRuntime.pak` beside it** (compatibility): for a repackaged runtime that keeps lmxxf's ABI v1 prefix. It gets only lmxxf's v1 functions and the 64-byte frame info every lmxxf-derived runtime reads the same way; a larger function table is accepted by size probing and only its v1 prefix is used.

`DLSS5_*` settings the runtime knows (found by name in the dll) are set in the process environment before the session is created; the menu greys out what the runtime does not read. A runtime with lmxxf's history ABI (sole export, `GetTimings`, `history_supported = 1`, the `temporal` branch of lmxxf) gets the 168-byte temporal frame info, `FULL_NETWORK` for the Full model, and its history network built during the warm-up.

Install: `<game>\OptiScaler\plugins\lmxxf_nr.asi` (OptiScaler's `MainDllPath` plugins folder) and in `OptiScaler.ini`:

```ini
[Plugins]
LoadAsiPlugins=true

[Plugin.lmxxf_nr]
Placement=1        ; 0 before upscaling, 1 after upscaling, 2 terminal (HUD-less, at present)
NetworkSize=0      ; 0 1080 tier, 1 2560x1440, 2 native (1 and 2 need DLSS5_NETWORK_FREE_RES)
Model=0            ; 0 Full (71 blocks), 1 Small (31 blocks), DLSS5_MULTI_PASS 1 / 0.5
Style=0            ; DLSS5_STYLE, 0 = NVIDIA's look
Strength=1.0
Temporal=true      ; motion-fed network history, when the runtime takes it
MultiGpu=false     ; see below, needs a restart
RuntimeDir=        ; folder of LmxxfNrRuntime.dll, tried first
ModulesDir=        ; HIP module folder (SHA256SUMS) of a runtime build
WeightsDir=        ; tiled weights of a runtime build
```

Motion vectors for the history are the game's, from the upscale callbacks, at the size the game gives them (render or output size; the runtime samples them over the feed's UV range, nearest texel): before and after upscaling straight from the evaluate, terminal from a copy taken in the evaluate of the same frame and used at its present, multi-GPU over the ring. History is reset on the game's reset flag, after a frame that was not fed, and when the vector extent or the network size changes.

## How it runs

- **Before / after upscaling** (upscale callbacks): the source (the render-size colour, or the upscaler's output) is box-filtered into the network size and fed (`PrepareFrame`, `RecordInputs`) on the game's own command list. When that list is submitted (an `ExecuteCommandLists` detour on the game's queue), the plugin's session queue waits for it and runs `EnqueueHip`; the game queue waits for the network only before the next frame's list that consumes it. The network's edit (its output minus what it was fed) is added to the next frame, moved by that frame's motion vectors; before upscaling the result is handed to the upscaler as its colour (`colorOverride`), after upscaling it is written into the output.
- **Terminal** (present callback, HUD-less request): NR on OptiScaler's HUD-less capture of the frame, linearised from the swapchain's encoding (sRGB, PQ or linear); the edit is added to the back buffer where it matches the HUD-less colour and the UI pixels stay exactly as drawn. Runs on the plugin's own list on the present queue, before OptiScaler's menu and overlay.
- **Multi-GPU** (`MultiGpu=true`): the game's device goes to the card without monitors (adapter override), the swapchain is a proxy whose buffers live there and whose real swapchain presents on the display card (swapchain provider); each frame (and its HUD-less capture and motion vectors) goes over a cross-adapter ring and the display card runs NR, composites the UI back and presents (flip, tearing). The `mgpu/` modules are the ones of the fork's multi-GPU branch, made host-independent. Keys: `MgpuHudless` (true), `MgpuNrEveryFrame` (true: the display card waits for this frame's answer), `MgpuMotionVectors` (true), `MgpuUiGate` (0.90: a HUD-less / back buffer pair differing on more than this share of pixels is taken as a wrong pairing and presented without NR, never NR over the UI), `MgpuMaxFramesAhead` (1), `MgpuRenderBus` / `MgpuDisplayBus` (-1 = auto), `MgpuPresenter`, `MgpuFallbackInline`, `MgpuDred` (false: DRED on the display device, logs the removed reason, page fault and unfinished lists at the first failed present).

Menu: a "Neural Rendering" tab (settings, runtime, multi-GPU and NR stats). FPS overlay (Detailed and fuller types): `NR: <network size> <model>, <GPU ms>` and, with multi-GPU, `MGPU: <load per card>, <added latency>, <NR ms>`.

Dumps: create `lmxxf_nr_dumps\lmxxf_nr_dump.trigger` in the OptiScaler folder: the next NR frame writes the network output, what it was fed and the result (`.raw` + `.bmp`); `mgpu_dump.trigger` there dumps multi-GPU frames. `lmxxf_nr_seq.trigger` (a frame count inside, default 12) dumps that many consecutive jobs: input, vectors and answer per frame plus `seq.json`; `tools/seq_metrics.py` scores flicker and ghosting on them.

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

## Build

`build.ps1` (VS 2022, v143, Release x64, logical threads - 2) builds `x64\lmxxf_nr\lmxxf_nr.asi` at the repository root. It uses OptiScaler's plugin SDK header (`src/plugin_sdk/`), lmxxf's MIT header (`third_party/lmxxf/`) and Microsoft Detours (MIT) as vendored by OptiScaler under `src/`.

## Credits

lmxxf's DLSS 5 on AMD runtime and C API: Kien (MIT). OptiScaler: cdozdil and contributors.
