# lmxxf_nr: DLSS 5 Neural Rendering for OptiScaler, as a plugin

An OptiScaler ASI plugin that runs lmxxf's DLSS 5 Neural Rendering runtime (`LmxxfNrRuntime.dll`, lmxxf's MIT C API `LmxxfNrApi.h`) around the upscaler or at present, on one GPU or on a second one. MIT licence (`LICENSE`).

It needs an OptiScaler with the plugin API extensions of this repository (`docs/PLUGIN_API_EXTENSIONS.md`): upscale callbacks (1) for the before / after placements, overlay text (3), the present callback (2) for the terminal placement and the HUD-less capture, and the adapter override (5) + swapchain provider (6) for multi-GPU. Each feature checks its API field and stays off without it.

## What the user installs

Nothing of the runtime ships with the plugin. It looks for a user-installed `LmxxfNrRuntime.dll` in `RuntimeDir` (setting), the game's exe folder, the OptiScaler folder and their `DLSS5-AMD\` subfolders, and for its assets:

- **lmxxf's own package layout** (first): `DLSS5-AMD\native-game-tiled-assets\HIP\SHA256SUMS` + the HIP modules, weights in `native-game-tiled-assets\`. The runtime gets the HIP folder as `assets_directory`. `DLSS5_*` settings it knows (found by name in the dll) are set in the process environment before the session is created.
- **A runtime + `LmxxfNrRuntime.pak` beside it** (compatibility): for a repackaged runtime that keeps lmxxf's ABI v1 prefix. It gets only lmxxf's v1 functions and the 64-byte frame info every lmxxf-derived runtime reads the same way; a larger function table is accepted by size probing and only its v1 prefix is used.

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
```

## How it runs

- **Before / after upscaling** (upscale callbacks): the source (the render-size colour, or the upscaler's output) is box-filtered into the network size and fed (`PrepareFrame`, `RecordInputs`) on the game's own command list. When that list is submitted (an `ExecuteCommandLists` detour on the game's queue), the plugin's session queue waits for it and runs `EnqueueHip`; the game queue waits for the network only before the next frame's list that consumes it. The network's edit (its output minus what it was fed) is added to the next frame, moved by that frame's motion vectors; before upscaling the result is handed to the upscaler as its colour (`colorOverride`), after upscaling it is written into the output.
- **Terminal** (present callback, HUD-less request): NR on OptiScaler's HUD-less capture of the frame, linearised from the swapchain's encoding (sRGB, PQ or linear); the edit is added to the back buffer where it matches the HUD-less colour and the UI pixels stay exactly as drawn. Runs on the plugin's own list on the present queue, before OptiScaler's menu and overlay.
- **Multi-GPU** (`MultiGpu=true`): the game's device goes to the card without monitors (adapter override), the swapchain is a proxy whose buffers live there and whose real swapchain presents on the display card (swapchain provider); each frame (and its HUD-less capture and motion vectors) goes over a cross-adapter ring and the display card runs NR, composites the UI back and presents (flip, tearing). The `mgpu/` modules are the ones of the fork's multi-GPU branch, made host-independent. Keys: `MgpuHudless` (true), `MgpuNrEveryFrame` (true: the display card waits for this frame's answer), `MgpuMotionVectors` (true), `MgpuUiGate` (0.90: a HUD-less / back buffer pair differing on more than this share of pixels is taken as a wrong pairing and presented without NR, never NR over the UI), `MgpuMaxFramesAhead` (1), `MgpuRenderBus` / `MgpuDisplayBus` (-1 = auto), `MgpuPresenter`, `MgpuFallbackInline`.

Menu: a "Neural Rendering" tab (settings, runtime, multi-GPU and NR stats). FPS overlay (Detailed and fuller types): `NR: <network size> <model>, <GPU ms>` and, with multi-GPU, `MGPU: <load per card>, <added latency>, <NR ms>`.

Dumps: create `lmxxf_nr_dumps\lmxxf_nr_dump.trigger` in the OptiScaler folder: the next NR frame writes the network output, what it was fed and the result (`.raw` + `.bmp`); `mgpu_dump.trigger` there dumps multi-GPU frames.

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

## Build

`build.ps1` (VS 2022, v143, Release x64, logical threads - 2) builds `x64\lmxxf_nr\lmxxf_nr.asi` at the repository root. It uses OptiScaler's plugin SDK header (`src/plugin_sdk/`), lmxxf's MIT header (`third_party/lmxxf/`) and Microsoft Detours (MIT) as vendored by OptiScaler under `src/`.

## Credits

lmxxf's DLSS 5 on AMD runtime and C API: Kien (MIT). OptiScaler: cdozdil and contributors.
