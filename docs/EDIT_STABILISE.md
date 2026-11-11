# Edit stabiliser (EditStabilise)

Guided edit upsample (EditUpsample=1) plus a temporal accumulation of the edit ratio at the output size.

## What it does
- History: RGBA16F ping-pong at the target size (ratio rgb, guide sqrt-luma a).
- Each frame, the history follows the motion vectors. It is dropped off screen and where the guide changed
  (tau 0.04).
- The history is clamped to R +- (1.0 * sd + 0.02), the JBU-weighted spread of the 3x3 feed ratios, then blended
  toward this frame's ratio with alpha 0.1.
- Wired into all three apply sites: upscale Apply, terminal ApplyTerminal (the HUD-less pass reads without writing),
  and whole-frame own lists.

The one arm kept: alpha 0.1 + spread clamp. Rejected:
- unclamped: more moving-edge error;
- box clamp, alpha 0.2: less flicker removed;
- no-luma guide: ghosting at disocclusions;
- the runtime's output_smooth: more moving-edge error and less flicker removed.

## Offline (numpy prototype vs the native network; ratios vs plain guided, lower is better)
| set | arm | PSNR | flk_mc | err_dis | err_edge | err_mov |
|---|---|---|---|---|---|---|
| mctl | guided | 45.42 | +.00080 | 1 | 1 | 1 |
| mctl | + runtime smooth | 45.09 | +.00056 | 0.99 | 1.03 | 0.99 |
| mctl | a.1 unclamped | 45.97 | +.00008 | 1.00 | 0.97 | 0.90 |
| mctl | **a.1 clamp** | 45.87 | +.00011 | 1.00 | 0.98 | 0.91 |
| mlego | guided | 45.68 | +.00181 | 1 | 1 | 1 |
| mlego | + runtime smooth | 45.36 | +.00068 | 1.05 | 1.05 | 0.99 |
| mlego | a.1 unclamped | 46.12 | +.00024 | 1.00 | 0.93 | 0.95 |
| mlego | **a.1 clamp** | 46.19 | +.00029 | 1.00 | 0.94 | 0.95 |
| sat4k | guided | 35.06 | +.00013 | 1 | 1 | 1 |
| sat4k | + runtime smooth | 35.06 | +.00003 | 1.02 | 1.03 | 1.02 |
| sat4k | a.1 unclamped | 35.32 | -.00044 | 1.00 | 0.94 | 0.93 |
| sat4k | **a.1 clamp** | 35.32 | -.00040 | 1.00 | 0.95 | 0.94 |
| satmv | guided | 34.89 | +.00048 | 1 | 1 | 1 |
| satmv | + runtime smooth | 34.40 | -.00010 | 1.04 | 1.10 | 1.22 |
| satmv | a.1 unclamped | 34.57 | -.00021 | 1.00 | 0.98 | 1.07 |
| satmv | **a.1 clamp** | 34.77 | -.00008 | 1.00 | 0.99 | 1.02 |

The test sets:
- mlego / mctl: synthetic 4K pans with a crossing patch and exact vectors. Re-measured 2026-11-10: the first table had
  the background's vectors with the wrong sign (the history was dropped by the guide test nearly everywhere, so the
  stabiliser showed little effect); with them corrected it gains 0.45-0.5 dB and removes 85 % of the moving flicker;
- sat4k: a Satisfactory 4K feed;
- satmv: Satisfactory 1080 vs 540 with game vectors.

The columns:
- flk_mc: output change minus input change along the vectors;
- err_dis: error at disocclusions;
- err_edge: error at moving edges;
- err_mov: error over the moving area.

Still frames: LEGO flk .00087 -> .00016 (FSR 3.1: .00016), PSNR 44.27 -> 45.05; CONTROL flk .00054 -> .00005.

## GPU
- upsample_bench --stab against the numpy prototype: 88-99 dB over 16 frames, maxabs <= 0.006 (the same as plain
  guided, fp16 rounding).
- Cost at 4K, bus 7: +0.15 ms (ApplyTerminal 0.664 -> 0.811 ms, Apply 0.661 -> 0.808 ms).

## In game (CONTROL menu, single GPU, terminal, Small 1080 + history)
Comparing EditStabilise false and true on one instance:
- edit transfer (mean abs(result - source), on / off): 0.943;
- network edit unchanged;
- UI pixels changed: 0 (no UI in this frame);
- failed presents 0, no device errors;
- 81.9 / 82.5 fps.

Still open: a gameplay check in motion with UI on screen, and a display-side sequence dump for flicker.
