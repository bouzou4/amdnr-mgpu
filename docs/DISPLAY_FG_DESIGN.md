# Frame generation on the display card: productising design

Status: prototype (branch `feat/displayfg2`, the spike ported onto the hang-fix presenter). This note says what the shipped feature would look like, what it costs and what has to be true before it ships.

## What it is

With `MultiGpu=true`, the game renders on the card without monitors and the display card presents. The display card has headroom: it is 40-60 % idle at the cheap NR presets. Display FG uses that headroom for FSR frame generation (FFX API, OptiScaler's own FidelityFX modules, provider 4.0.2). The presenter's real swapchain becomes an FFX frame generation swapchain on the display card. Each real frame arrives over the ring with its depth (plane 0, R32) and motion vectors, and FFX generates one frame between each pair of real frames. The render card does no extra work beyond sending depth.

UI: the presenter already has the frame's HUD-less colour (after NR) and the back buffer with UI. The HUD-less colour goes to FFX as `HUDLessColor`, so the generated frame interpolates the scene only and the UI is copied from the real frame.

## Data path per real frame

1. Render card: colour, HUD-less colour, MVs, depth plane 0 into the ring slot (depth is ~12 MB at 2304x1296 D32S8, ~0.8 ms of render time by the 0.065 ms/MB rule).
2. Display card `_dq`: ingest, NR, UI composite into the FFX swapchain's back buffer (unchanged path), queued only once the CPU has seen the send land.
3. The presenter waits on the CPU until `_dq` is done with the frame. Then FG prepare (depth, MVs, camera) runs on `_fq` (FFX's game queue), then configure (HUD-less colour, present callback) and the real Present through the FFX swapchain.
4. FFX's present thread presents generated, then real, paced by FFX.
5. Before the next frame is queued, the presenter waits on the CPU until `_fq` is past this Present: FFX reads the HUD-less colour, depth and vectors the next ingest overwrites.

No queue of ours waits on a fence (the display-hang rules): both hand-overs between `_dq` and `_fq` are CPU waits. FFX's own queues wait on each other inside its swapchain, as in any game with FSR frame generation.

`_fq` is separate from `_dq` so FFX's work never shares a queue with the presenter's. Measured in the spike: pacing is the same on either queue once it settles.

## Settings (shipped shape)

One row: `Frame generation (display card)`: off / on. It only shows when multi-GPU is active and the host exposes the FFX API. It is off automatically while the game's own frame generation is on (the game's FG presents through the proxy).

`MgpuFgVersion` picks the FG provider by version prefix (`3` = FSR 3.1 frame generation of OptiScaler's FidelityFX module, default = the newest, which is the driver's FSR 4 ML frame generation on RDNA 4).

`MgpuFgSwapOnDemand` (default true) makes the real swapchain the FFX one only while FG is on, and a plain one otherwise, swapped on the game's thread between two frames (frames in flight are dropped, colour space and HDR metadata are set again). It removes the FFX swapchain's cost with FG off (1.1-3.1 ms per frame when the display card is the bound) for a 0.2-0.6 s hitch at each FG toggle.

Everything else stays an ini key for diagnostics, not a menu row: `MgpuFgHudless`, `MgpuFgAsync`, `MgpuFgOwnQueue`, `MgpuFgPacing*`, `MgpuFgCamera*`, `MgpuFgVerticalFov`, `MgpuFgSynthetic*`, `MgpuFgDebug`.

Camera near/far/FOV: FFX uses them only for its depth-based disocclusion heuristics. Defaults (0.1 / 100000 / 60°) are acceptable. A per-game value can come from the upscaler's camera fields when the game passes them (OptiScaler has them for FSR/XeSS inputs), so no user knob is needed.

## Requirements before shipping

| Gate | How it is measured | Status |
|---|---|---|
| No display device loss, FG on, NR on and off, 10-minute run | in-process log, DRED page faults | see Results |
| Even pacing: every presented frame on screen for half the real frame time, within 15 %, p95 | PresentMon displayed time (the `fg pacing:` call times do not show it under flip metering) | harness: yes (5.42 ms, p05 4.90, p95 5.95) |
| Latency added by FG at most one real frame + 2 ms | PresentMon display latency against FG off | not measured at display level yet |
| UI pixels in generated frames equal the real frame's UI | `fg_compare.py` on the dumps (UI mask from display vs HUD-less) | see Results |
| Generated frame closer to the neighbours' mean than to either neighbour | `fg_compare.py` | harness: yes |
| Resize, fullscreen, display card loss, minimise: the same behaviour as without FG | the README robustness checks, rerun with FG | not run |

## Results

Harness (3840x2160, Performance preset, synthetic depth and vectors, no render load), on the hang-fix presenter:

| Test | Result |
|---|---|
| FG on, provider 4.0.2 | 92-94 real frames/s, 184-187 displayed/s |
| Cost of the FFX swapchain, display-card bound, both links at 28 GB/s, one batch | plain swapchain 8.80 ms per frame (113.6 fps); FFX swapchain with FG off 11.87 ms (84.2); with FG on 10.66 ms (93.8 real). Presenter time on the FG path: the CPU wait for its own queue before FFX's Present 1.1-1.3 ms (FFX copies the back buffer on its own queue, so that work has to be done first), the Present call 0.2-0.5 ms, with FG on also prepare + configure 0.7 ms and the wait for FFX's queue 0.7 ms. With FG off the network itself also runs slower (6.44 ms against 4.75 with the plain swapchain or with FG on); seen in three of four FG-off runs, cause not found. Flip metering off (`[FSRFG] DisableFlipMetering`) changes nothing |
| Same with bus 7 at 14 GB/s | plain 10.50 ms, FG off 11.63, FG on 11.69; network 4.7 ms in all three |
| `MgpuFgSwapOnDemand=true` | the real swapchain is the FFX one only while FG is on: with FG off the frame time is the plain swapchain's (8.7-8.8 ms at 28 GB/s). Swapping takes 540-604 ms the first time (FFX swapchain and context), then 161-185 ms; four live swaps in one run, clean. No swap in exclusive fullscreen or within 120 frames of the last one; a failed FFX swapchain leaves FG off |
| Upscaler start with a HUD-less colour of a wrong pairing, gate frames, the right pairing, upscaler stop and start, with 1.7 s render stalls (`mgpu_harness --upscaler --hudless --upscaler-late --script`), FG on, 4.0.2 and 3.1.6 | clean; the FG context is rebuilt when the render size appears |
| Display pacing, FG on (PresentMon, 50 s) | each presented frame on screen 5.42 ms (p05 4.90, p95 5.95), none dropped, Independent Flip. The Present calls come in pairs 0.2 ms apart: the driver's flip metering spaces the flips, so call times say nothing about pacing |
| Generated frame vs its neighbours (`fg_compare.py`), 4.0.2 | 31.5 dB against the neighbours' mean, 27.2 / 29.1 against each neighbour (the neighbours 24.6 apart): an interpolation |
| Same, provider 3.1.6 | 33.8 dB against the mean, 28.8 / 28.9 against each neighbour |
| Generated frame equal to the next real one (seen after the 4.0.2 call order change) | the harness passed an all-zero HUD-less colour: the whole frame was UI and FFX kept it as drawn. Both call orders and both providers showed it. The harness now passes the frame itself |
| Render queue stalls of 2.5 s (NR on, NR off) and 3.5 s with FG on | no device loss; the send wait logs each stall, nothing is queued meanwhile |
| FG on / off live, provider 4.0.2 <-> 3.1.6 live | clean (the context is rebuilt) |
| Multi-frame (numGeneratedFrames 2 and 4, own outputs[1..]) | spike result: dispatch returns OK, outputs[1] stays all zero: 4.0.2 generates one frame. Dropped, the probe is removed |

Satisfactory (spike, before the port): dfg4 and dfg5 lost the display device about 0.5 s after FG came on (provider 4.0.2, page fault at a VA DRED could not attribute). Not HUD-less, render scale or the queue. Not rerun yet on this branch; the jobs that separate provider (3.1.6 against 4.0.2) and inputs (synthetic against the game's) are in the rig queue.

## With the asynchronous network (NrCadence=async, this branch)

Harness, Quality, provider 4.0.2, synthetic depth and vectors, `MgpuDred=1`, both links at 28 GB/s:

| Case | real fps | presented/s | notes |
|---|---|---|---|
| async, no FG | 540 | 540 | the harness has no render load |
| async + FG | 115 | 230 | PresentMon: each frame on screen 3.88 ms median (p95 4.91), display latency 9.4 ms median, one 548 ms hitch at FG start |
| every-frame NR + FG | 88.8 | 178 | the presenter waits 15 ms per frame for the network |
| async + FG, the upscaler-start / HUD-less gate sequence with 1.7 s render stalls | | | clean: FG generates throughout, its context is rebuilt when the upscaler's render size appears |

Generated frame against its neighbours: 31.4 dB against their mean, 26.9 / 28.8 against each (an interpolation).

Satisfactory (on feat/displayfg2, every-frame NR, `MgpuDred=1`, no dumps): FSR 3.1.6 and 4.0.2 both ran synthetic inputs, then the game's depth, vectors and HUD-less colour, then NR on, with no loss: 63-64 real fps, 128-129 presented per second. The spike's 4.0.2 losses (dfg4, dfg5) ran with DRED page faults only, without breadcrumbs. These passes ran with breadcrumbs (`MgpuDred=1`), the interim mitigation for the display-card losses, so they do not clear 4.0.2 without it.

Games on this branch (cold launches, `MgpuDred=1`, no dumps, uncapped; FG off at start and switched on live, so the real swapchain is swapped to the FFX one on demand, 0.24-0.32 s). Real fps from the rig's frame log; presented/s and game->DXGI (game Present to the real Present of the frame) from the plugin's `fg:` lines; pacing is the gap between DXGI Presents (generated->real / real->generated, medians). No PresentMon on the rig: with AMD flip metering on, these are submit times, not scanout.

| Game, preset | phase | real fps | presented/s | game->DXGI | pacing |
|---|---|---|---|---|---|
| Satisfactory Quality | async | 64.1 | 64 | 15.0 ms (stats latency) | |
| | async + FG 3.1.6 | 65.4 | 123-126 | 19.0 ms | 0.4 / 14.6-17.9 ms |
| | async + FG 4.0.2 | 62.1 | 120-122 | 19.9 ms | 0.4 / 14.6-14.7 ms |
| Satisfactory Ultra | async | 60.2 | 60 | 14.9 ms (stats latency) | |
| | async + FG 3.1.6 | 54.0 | 102-116 | 20.5-22.9 ms | 0.3 / 17.6-18.0 ms |
| | async + FG 4.0.2 | 52.8 | 99-114 | 21.6-23.7 ms | 0.4 / 15.8-15.9 ms |
| Hellraiser Quality, FSR input | async | 85.3 | 85 | 11.5 ms (stats latency) | |
| | async + FG 3.1.6 | 82.5 | 154-165 | 14.9-15.9 ms | 0.4 / 11.6-11.8 ms |
| | async + FG 4.0.2 | 82.9 | 162-166 | 15.4-15.8 ms | 0.4 / 11.3-11.5 ms |

No loss or failed present in the four launches. Hellraiser's DLSS input (`UpscalerOption=3`, the one the earlier Hellraiser jobs used) carries no depth: OptiScaler logs `Depth not exist` on every evaluate, FG is on with no generated frames (the plugin now says so once). Its FSR input (`UpscalerOption=2`) carries depth. The generated frame goes out 0.3-0.4 ms before the real one and the real one 11-18 ms after it, the same pattern as on the hang-fix presenter; whether flip metering spreads them at scanout needs PresentMon or a capture card.

## Failure handling

- FFX swapchain creation fails: plain swapchain, FG row greyed out with the reason.
- FG context or prepare fails: FG off for that frame, logged once. The FFX swapchain presents the real frame (passthrough).
- Display device lost: the existing fallback (render-card present, NR off) is unchanged, and FG goes off with it.
- Resize: `ReleaseContext` (configure off, wait for presents) before `ResizeBuffers`; the context is rebuilt at the new size on the next frame.
- Window/swapchain size mismatch (`sizeHold`): FG is held like NR.

## Cost

- Render card: depth plane 0 in the send (~0.8 ms per frame at 4K output with a 2304x1296 render size). Depth goes only when FG is on.
- Display card: prepare + generation, plus one extra present per real frame.
- Latency: FFX holds each real frame until its generated frame was shown, so about one real frame time is added (harness: 9.5 -> 19.1 ms from the game's present to the DXGI present).

## Risks

- FFX pacing settles only after a few seconds. In 3 of 6 harness runs, the first 10-20 s showed generated and real frames presented back to back (~0.3 ms apart). FRAMEPACINGTUNING is exposed for testing.
- The FG swapchain wraps the presenter's real swapchain. Every DXGI call the presenter makes (colour space, HDR metadata, fullscreen) now goes through FFX's wrapper. HDR (PQ) works in the harness; scRGB is not tested.
- The provider is OptiScaler's FidelityFX module. A host without the FFX API (plugin API ext 7) gets no row.
