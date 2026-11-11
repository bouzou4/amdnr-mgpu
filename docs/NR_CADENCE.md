# NR cadence (NrCadence)

Spike result. The network no longer has to run on every displayed frame: `NrCadence=2|3|async` (default `1`), terminal and multi-GPU placements with the guided upsample (`EditUpsample=1`).

## What it does
- One network job in flight at most. Its answer goes onto the frame it arrives on; frames without an answer show the stabiliser's history (the edit as a ratio at the output size) moved by their motion vectors.
- **Accumulate pass**: per pixel, the uv offset from this frame to the frame last fed (this frame's vector plus the accumulated offset where it points; a pixel that left the screen stays unknown). RG16F at the vectors' size.
- **Answer frame**: the pair (feed, answer) is read at that offset with the guided 3x3, dropped where no feed pixel there looks like the pixel (disoccluded since), then blended into the history as on an every-frame answer.
- **Other frames**: the history where it holds (on screen, guide unchanged). Pixels it lost take the ratio of last frame's pixels around them that looked alike (16 taps, the history keeps its guide), with the reprojected ratio as a floor.
- **The network's own history** gets the accumulated offset as its motion input, so it spans the skipped frames.
- **Feed at the output size** (Ultra): the history is used under cadence too; the pair's own ratio, the 3x3 only for the clamp's spread.
- `2` / `3`: fed every 2nd / 3rd frame; a fed frame waits only when the last answer is older than that.
- `async`: fed again as soon as the last answer is in; no frame waits. With multi-GPU the presenter never waits for the network.
- **Guard**: on every 4th frame, every 8th pixel's guide is compared with last frame's in the history, at the moved position and in place. Vectors that are wrong (the moved read misses at least 0.9 times as often as the in-place one, on 1 % of the frame or more) or 15 % of the frame changing in place hold the cadence back: the every-frame path runs until the checks have been clean for 2 s.
- Not with the game's FG input edited in place (that path keeps the network on every frame). Live through `lmxxf_nr_ini.trigger`.

## Offline quality (numpy prototype on recorded sequences, half-size feed)
P = frames between network runs, d = frames until the answer lands; for P 2 and 3 the network was run on every P-th frame with composed vectors. PSNR against the native-size network run on every frame (loss to the same-frame answer in brackets); flicker = output change minus input change on still / moving pixels (display encoding); edge = error on moving edges over the same-frame answer's.

| set | arm | PSNR | flicker still | flicker moving | edge |
|---|---|---|---|---|---|
| Satisfactory, 2.5 px/frame | same-frame answer | 34.77 | +.00002 | -.00008 | 1 |
| | P1 d1 | 34.40 (-0.36) | +.00036 | +.00016 | 1.02 |
| | P2 d1 | 34.08 (-0.69) | +.00033 | +.00026 | 1.03 |
| | P2 d2 | 33.86 (-0.91) | +.00033 | +.00048 | 1.06 |
| | P3 d3 | 33.45 (-1.32) | +.00027 | +.00045 | 1.18 |
| | terminal as it ships (last frame's pair, not moved) | 33.94 (-0.83) | +.00037 | +.00024 | 1.51 |
| Satisfactory, 5 px/frame | same-frame answer | 34.21 | -.00123 | -.00017 | 1 |
| | P1 d1 | 33.62 (-0.59) | +.00158 | +.00006 | 1.00 |
| | P2 d1 | 33.66 (-0.54) | +.00044 | +.00007 | 1.05 |
| | P2 d2 | 33.42 (-0.79) | +.00049 | +.00042 | 1.08 |
| | P3 d3 | 32.93 (-1.28) | +.00042 | +.00022 | 1.14 |
| | terminal as it ships | 33.00 (-1.21) | +.00160 | +.00025 | 1.52 |
| DX11 game, 25 px/frame | same-frame answer | 42.06 | -.00053 | +.00007 | 1 |
| | P1 d1 | 42.08 (+0.02) | -.00026 | +.00022 | 1.03 |
| | P2 d2 | 42.26 (+0.20) | -.00030 | +.00023 | 1.13 |
| | P3 d3 | 42.14 (+0.08) | -.00033 | +.00016 | 1.20 |
| | terminal as it ships | 40.86 (-1.20) | +.00002 | +.00067 | 1.67 |
| Synthetic pan, patch at 18 px/frame | same-frame answer | 42.31 | - | +.00041 | 1 |
| | P1 d1 | 41.91 (-0.40) | - | +.00049 | 1.00 |
| | P2 d2 | 41.36 (-0.95) | - | +.00025 | 0.99 |
| | P3 d3 | 40.84 (-1.47) | - | +.00021 | 1.02 |
| | terminal as it ships | 38.54 (-3.78) | - | +.00056 | 1.91 |
| Animated menu, still camera | same-frame answer | 40.88 | +.00103 | - | - |
| | P1 d1 | 32.63 (-8.25) | +.00200 | - | - |
| | P3 d3 | 33.27 (-7.61) | +.00007 | - | - |
| | terminal as it ships | 32.32 (-8.56) | +.00235 | - | - |

- With motion and correct vectors a late answer costs 0 to 1.5 dB up to P3 d3, about 0.3-0.4 dB per frame of delay on the Satisfactory sets, with moving-edge error up 0-20 %.
- A late answer loses the stabiliser's steadiness on still pixels: flicker goes back to the raw network's level (+.0003 to +.0005; +.0016 at P1 d1 on the second set, where every frame blends in a ratio fitted to the previous frame's shimmer).
- The image pulses a little at the network rate: frames where an answer lands change by .0002-.0010 more than the frames between (display units, mean over the frame; .003 on the animated menu).
- Content that changes without moving (the animated menu) is the weak case for any late answer: 7-9 dB under the same-frame answer.
- The single-GPU terminal path as it ships already applies last frame's answer, unmoved. The cadence path at P1 d1 is better than it on every set; P2 d2 is level or better on PSNR with 30-50 % less moving-edge error.
- With a native-size feed, P3 d3 stays 36-37 dB from the every-frame native network on the Satisfactory sets, closer to it than a half-size feed run on every frame (34-35 dB).
- What the pieces are worth (P2 d2): without the network's history across the gap flicker doubles; plain reprojection instead of the fill gives 1.6x the disocclusion error on the synthetic pan, no edit there 1.7x on Satisfactory; without the stabiliser's blend at the native size the pulse is 2-3x.
- Not kept: re-referencing a late answer to the current frame's feed-size colour. It brings the still flicker back to the every-frame level but holds stale content: 2.5-3.7 dB lost on the animated menu, 0.5-0.9 dB at 25 px/frame.
- It depends on the motion vectors. On the synthetic sets with the background's vectors of the wrong sign, P1 d1 lost 2.8-5.6 dB and P3 d3 5.2-9.3 dB, while the same-frame answer lost nothing.

### The guard on the same sequences
Held frames take the every-frame path: with multi-GPU that is the frame's own answer, on one GPU the terminal path as it ships.

| set | checks flagged | P3 d3 | P3 d3 with the guard, multi-GPU | one GPU |
|---|---|---|---|---|
| Synthetic pans, wrong-sign vectors | every one (moved read misses 1.2 times as often as in place) | -5.2 / -9.3 dB | -0.2 / -0.0 dB | -3.6 / -3.1 dB, the shipped path's level |
| Animated menu | its bursts (27-31 % of the frame changes in place) | -7.6 dB | +0.1 dB | -8.5 dB, the shipped path's level |
| Satisfactory 2.5 and 5 px/frame, DX11 game 25 px/frame | none (0.02-0.44 times; under 7 % in place) | unchanged | unchanged | unchanged |

- On one GPU nothing cheap helps content that changes in place: every path there applies a late answer, and dropping the edit on changed pixels is worse than the stale one (-11.4 against -8.7 dB on the menu).
- In the Satisfactory menu the guard holds for 2 s at the scene cuts (the vectors mean nothing across a cut) and nowhere else.

## GPU
- `upsample_bench --cadence P,D` against the numpy prototype: 88-96 dB over 6 frames (cadence 3,2 at 4K), as for the stabiliser alone.
- Cost at 4K on one R9700: held frame 0.74 ms, answer frame 0.95-0.97 ms (every-frame stabilised apply: 0.86 ms), accumulate 0.21-0.24 ms at 4K vectors. Native-size feed: 0.78 / 1.21 ms. The guard's check: 0.25-0.3 ms on every 4th frame; its sums match the prototype's (wrong sign 3.6 % against 2.9 %, corrected 0.1 % against 2.9 %).

## Satisfactory, one GPU, 4K (rig: real HUD-less colour and vectors)
| | NR off | every frame | cadence 2 | cadence 3 | async |
|---|---|---|---|---|---|
| Quality, fps | 62.0 | 32.9 | 42.4 | 46.7 | 46.2 |
| Quality, p99 frame ms | 18.5 | 36.1 | 28.7 | 30.6 | 30.9 |
| Ultra, fps | 62.0 | 18.5 | 29.1 | 35.1 | 39.1 |
| Ultra, p99 frame ms | 19.0 | 59.2 | 47.0 | 34.0 | 34.4 |

- Async: no frame waits; the network lands every 3rd frame at Quality and every 4th at Ultra.
- Gates pass at both sizes with the dump taken in async: UI pixels unchanged (0.000 %), edit transfer faithful, feed in [0, 1], no failed present. The network's answers, fed the accumulated vectors, stay steadier than their input (moving flicker -0.0019 / -0.0003).
- Cadence 2 / 3 were measured one build earlier (before the guard); they have no advantage over async here.

## Harness (4K, no motion vectors, emulated game load; NR off 112-124 fps)
| | every frame | cadence 2 | cadence 3 | async |
|---|---|---|---|---|
| Single GPU, Quality | 37.7 fps | 56.5 | 65.8 | 95.2 |
| Single GPU, Ultra | 20.7 | 34.9 | 44.7 | 93.9 |
| Multi-GPU, Quality | 49.6 | 97.4 | 124.3 | 124.2 |
| Multi-GPU, Ultra | 24.3 | 47.2 | 68.1 | 117.5 |

- Multi-GPU async: the NR-off frame rate and latency (presenter 8.0-8.5 ms, answer wait 0.00 ms); the network lands every 3rd (Quality) or 5th (Ultra) frame at this frame rate.
- Cadence 2 / 3 wait whenever the network takes longer than 2 / 3 frames, and the fed frame carries the whole wait: frame times alternate (single GPU Quality, cadence 2: median 19 ms, consecutive frames differ by 12 ms on average).
- Single GPU async paces evenly (p99 14 ms) but the game takes the GPU first: the network lands only every 5th (Quality) to 8th (Ultra) frame, outside the offline study's range.

## Open
- Multi-GPU game runs were parked while this was written, after display-card losses at the loading-to-menu transition that also hit other builds.
- Single GPU: a pacer for cadence 2 / 3. Async in the game needs no floor on the network rate (every 3rd-4th frame); in the harness it did (every 5th-8th).
- The guard's hold at scene cuts could be skipped when the game flags the cut.
- The every-frame terminal path could take the moved read of last frame's pair (P1 d1 above).
- The stabiliser at the native size (every frame): 48-56 dB from the unstabilised answer, its still flicker a tenth to a third.
