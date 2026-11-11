# Frame generation on the display card: productising design

Status: spike (branch `spike/displayfg`). This note says what the shipped feature would look like, what it costs and what has to be true before it ships.

## What it is

With `MultiGpu=true`, the game renders on the card without monitors and the display card presents. The display card has headroom: it is 40-60 % idle at the cheap NR presets. Display FG uses that headroom for FSR frame generation (FFX API, OptiScaler's own FidelityFX modules, provider 4.0.2). The presenter's real swapchain becomes an FFX frame generation swapchain on the display card. Each real frame arrives over the ring with its depth (plane 0, R32) and motion vectors, and FFX generates one frame between each pair of real frames. The render card does no extra work beyond sending depth.

UI: the presenter already has the frame's HUD-less colour (after NR) and the back buffer with UI. The HUD-less colour goes to FFX as `HUDLessColor`, so the generated frame interpolates the scene only and the UI is copied from the real frame.

## Data path per real frame

1. Render card: colour, HUD-less colour, MVs, depth plane 0 into the ring slot (depth is ~12 MB at 2304x1296 D32S8, ~0.8 ms of render time by the 0.065 ms/MB rule).
2. Display card `_dq`: ingest, NR, UI composite into the FFX swapchain's back buffer (unchanged path).
3. `_dq` signals, `_fq` (FFX's game queue) waits on it. FG prepare (depth, MVs, camera) runs on `_fq`, then configure (HUD-less colour, present callback) and the real Present through the FFX swapchain.
4. FFX's present thread presents generated, then real, paced by FFX.

`_fq` exists so FFX never queues behind `_dq`'s waits on the HIP answer. Measured: pacing is the same on either queue once it settles, so `_fq` is kept for isolation, not for speed.

## Settings (shipped shape)

One row: `Frame generation (display card)`: off / on. It only shows when multi-GPU is active and the host exposes the FFX API. It is off automatically while the game's own frame generation is on (the game's FG presents through the proxy).

Everything else stays an ini key for diagnostics, not a menu row: `MgpuFgHudless`, `MgpuFgAsync`, `MgpuFgOwnQueue`, `MgpuFgPacing*`, `MgpuFgCamera*`, `MgpuFgVerticalFov`, `MgpuFgSynthetic*`, `MgpuFgDebug`.

Camera near/far/FOV: FFX uses them only for its depth-based disocclusion heuristics. Defaults (0.1 / 100000 / 60°) are acceptable. A per-game value can come from the upscaler's camera fields when the game passes them (OptiScaler has them for FSR/XeSS inputs), so no user knob is needed.

## Requirements before shipping

| Gate | How it is measured | Status |
|---|---|---|
| No display device loss, FG on, NR on and off, 10-minute run | in-process log, DRED page faults | see Results |
| Even pacing: generated->real and real->generated within 15 % of half the frame time, p99 | `fg pacing:` log line | harness: yes after start-up |
| Latency added by FG at most one real frame + 2 ms | `fg:` log, game present -> DXGI present | harness: +9.6 ms at 100 fps (one frame) |
| UI pixels in generated frames equal the real frame's UI | `fg_compare.py` on the dumps (UI mask from display vs HUD-less) | see Results |
| Generated frame closer to the neighbours' mean than to either neighbour | `fg_compare.py` | harness: yes |
| Resize, fullscreen, display card loss, minimise: the same behaviour as without FG | the README robustness checks, rerun with FG | not run |

## Results

| Test | Result |
|---|---|
| Harness, FG on (synthetic depth/motion, HUD-less, 0.6 scale) | FG runs; pacing settles to 4.8/5.0 ms gaps after a 10-20 s start-up collapse; +9.6 ms latency at 100 fps |
| Satisfactory, FG on (dfg4, dfg5) | display device hang ~1 s after FG on, DRED page fault at an unowned VA; not HUD-less, render scale or queue |
| Satisfactory input isolation (dfg6: synthetic depth / motion / real / old call order) | not run: the display card was in a post-TDR state |
| Multi-frame (numGeneratedFrames 2 and 4, own outputs[1..]) | dispatch returns OK, outputs[1] stays all zero: 4.0.2 generates one frame. MFG dropped |
| Generated frame vs neighbours, builds before the 4.0.2 call order | closer to the neighbours' mean (31-32 dB) than to either neighbour (25-29 dB) |
| Generated frame vs neighbours, 4.0.2 call order + multi-frame probe | bit-identical to the next real frame: no interpolation. Open: probe or call order |
| UI pixels in generated frames | not scored: needs game dumps (dfg6) |

After a display TDR, every later D3D12 harness start on the display card hangs before the first log line until a reboot. Any FG result taken after a TDR in the same boot is invalid.

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
