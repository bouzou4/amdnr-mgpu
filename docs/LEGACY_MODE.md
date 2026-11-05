# Legacy mode: Neural Rendering in games without upscaler hooks

Status: spike (`spike/legacy`). First target: Half-Life 2: Episode Two (Source 2013, Steam). Code: `plugins/lmxxf_nr/legacy/`.

## 1. The target

| Fact | Evidence |
|---|---|
| Ep2 ships inside app 220 (Half-Life 2, 20th anniversary update) and runs as `hl2.exe -game ep2`; about 10 GB on disk | Steam app manifests 220/380/420; [anniversary update notes][hl2-20th] |
| **32-bit only on Windows**: `hl2.exe` and every `bin\*.dll` are PE32 (i386), no 64-bit build | PE headers of the installed build; PCGamingWiki lists the 64-bit Windows build as no longer available ([PCGW][pcgw-hl2]) |
| Default renderer: `bin\shaderapidx9.dll`, which imports `Direct3DCreate9` from `d3d9.dll` (import table) and uses **D3D9Ex** (the `-nod3d9ex` switch is in the binary) | `dumpbin /imports`; strings |
| `-vulkan`: `bin\shaderapivk.dll` loads the bundled 32-bit `bin\dxvk_d3d9.dll` (DXVK, D3D9 on Vulkan) | strings; [Valve Developer Wiki, DXVK][vdc-dxvk]; [Steam discussion][steam-vk] |
| OptiScaler has no D3D9 path, and its builds are x64 | `src/OptiScaler/proxies` (DXGI, D3D12, Vulkan, NGX, FFX, XeSS, Streamline only) |

The bitness decides the design. Our plugin, OptiScaler and lmxxf's HIP runtime are all x64, and HIP has no 32-bit runtime, so **NR can't run inside the game process**. Many of the old games this mode is for are 32-bit (most D3D9 titles and many D3D11 ones), so the out-of-process split below is the general design, not an Ep2 workaround.

## 2. Getting frames into D3D12: decision

| Option | What it takes | Verdict |
|---|---|---|
| (a) **D3D9On12** (Microsoft's D3D9 to D3D12 mapping layer, in Windows since 10 2004, [MIT source][9on12-gh]) | A `d3d9.dll` proxy calls `Direct3DCreate9On12(Ex)` with *our* D3D12 device and queue (`D3D9ON12_ARGS`). The game's back buffer is then a D3D12 resource: `IDirect3DDevice9On12::UnwrapUnderlyingResource` / `ReturnUnderlyingResource` check it out to our queue with fence sync ([interop spec][interop-spec], [DirectX blog][interop-blog]). | **Chosen.** No copy through another API, explicit GPU sync, our own device (we choose the adapter, which matters for multi-GPU). OptiScaler's D3D12 hooks are *not* used, because 9On12 does not present through DXGI and OptiScaler can't load into a 32-bit process anyway. |
| (b) DXVK (`-vulkan`) plus our Vulkan path (spike/api11vk) | The api11vk path rides OptiScaler's Vulkan hooks, which are x64. In a 32-bit game it would also need the cross-process hop, via Vulkan external memory. | Possible later for games where 9On12 breaks. It adds a third API with nothing gained for D3D9. |
| (c) Native D3D9Ex plus shared handles | D3D9Ex shares only with legacy (non-NT) handles: StretchRect into a shared render target, open it in D3D11 (`OpenSharedResource`), bridge to D3D12, and sync on the CPU (event queries, no fences). The result has to come back the same way. | Fallback, for when 9On12 renders a game wrongly. Two extra copies, CPU sync. |
| (d) Window capture (Magpie, [GPL-3.0][magpie]) | Desktop or window capture plus an overlay window. | Last resort: a frame or more of latency, captures the composed desktop, GPL (reference only). |

The prototype does (a) together with the process split:

```
 32-bit game (hl2.exe)                                   x64 host (lmxxf_legacy_host.exe)
 shaderapidx9 -> d3d9.dll proxy -> D3D9On12 ---------+
                 our D3D12 device + queue            |
 Present hook:                                       |
   Unwrap back buffer (COMMON) on our queue          |
   copy -> shared texture  ------------------------------> opened by name (NT handle)
   Signal shared fence 2n-1 ---------------------------> Wait 2n-1
                                                        OpticalFlow (luma pyramid, block match)
                                                        NrEngine::RunOwn (whole frame, this frame's answer)
   CPU wait 2n (timeout 250 ms) <----------------------- Signal 2n
   copy shared texture -> back buffer
   ReturnUnderlyingResource(fence)
   real Present
```

- **Kernel objects**: named NT handles (`ID3D12Device::CreateSharedHandle` with a name, `OpenSharedHandleByName` in the host) for the frame texture (recreated with a generation number on resize) and one shared fence, plus a small named file mapping (`legacy/LegacyShared.h`, the same layout in both builds) for size, format, LUID, generation, reset and host status. Both devices are on the same adapter (the proxy writes the LUID, and the host opens that adapter).
- **Proxy fixes on the device parameters**: MSAA is forced off (NR needs one sample). X8R8G8B8 becomes A8R8G8B8: BGRX has no typed UAV, and the result is written by a compute pass.
- **Robustness**: the proxy waits on the CPU with a timeout, never a GPU wait on a value the host might never signal. A host that dies or stalls costs one 250 ms frame, after which frames go straight to the screen (`g_stalled` until the host catches up). The host exits with the game. A `proxy_bypass.trigger` file in the dump folder turns the round trip off live (fps baseline).
- **Ownership**: the proxy starts the host as a child process (`HostExe` in `lmxxf_legacy.ini` beside the proxy), so a rig that kills its own game PID tree also kills the host.

## 3. Temporal: motion vectors from colour

The runtime needs only colour. Its motion-fed history (`TEMPORAL.md` in lmxxf-nr) halves flicker on game frames, but it needs vectors (current to previous, `previous = current + mv`). Legacy games give none.

Prior art (all whole-frame NR on games without hooks):

| Project | Capture | Motion vectors | Licence |
|---|---|---|---|
| lmxxf's DLSS 5 on AMD, Magpie path ([repo][lmxxf]) | Magpie window capture | none ("the Magpie path has no game motion vectors"); AMD optical flow is used by its FSR 3 frame-generation item, not fed to NR | MIT (runtime); Magpie GPL-3.0 |
| Magpie Experimental ([Igor's Lab test][igor]) | window capture | estimated from consecutive colour frames; ghosting and shimmer reported | GPL-3.0 (Magpie) |
| DLSS5-Feeder ([mirror][feeder]) | ReShade add-on | "estimated motion vectors" plus ReShade depth, method not documented | MIT |
| iMMERSE Launchpad ([repo][immerse]) | ReShade | dense block-matching motion estimation | proprietary (all rights reserved): reference only |
| FidelityFX optical flow ([GPUOpen manual][ffx-of]) | (FSR 3 frame generation) | 8x8 blocks, `R16G16_SINT`, scene-change flag, no per-pixel confidence | SDK MIT for listed files, prebuilt binaries under AMD's licence ([licence][ffx-lic]) |

Choice for the prototype: **our own MIT block matcher** (`legacy/host/OpticalFlow.cpp`).

- Luma at up to 960 pixels wide, a 4-level pyramid.
- 8x8 blocks searched coarse to fine: ±6 pixels at the top level, then the doubled parent vector and its 4 neighbours as candidates, refined by ±2 and a parabola for the sub-pixel part. A zero-vector bias of 0.95 keeps flat areas still.
- A per-pixel pass picks the best of the 3x3 neighbouring block vectors on a 3x3 window, so vectors follow edges.
- No dependency, any GPU, one file.

FidelityFX optical flow was measured against it and dropped (section 6.1). Wrong vectors are bounded by the runtime itself: its raw guard rejects history wherever the warped previous input differs from the current input by more than 0.08, so bad flow degrades to "no history" locally rather than to ghosting.

Results: see section 6.

## 4. UI

Whole-frame NR includes the HUD; that is the prototype. For clean captures the rig hides it with `cl_drawhud 0` (needs `sv_cheats 1`) and `r_drawviewmodel 0`. For players, options are, in order of cost:

1. A static-pixel UI mask: the optical flow already marks pixels whose vector is zero and whose luma is stable while the scene around them moves. Over a few hundred frames that classifies HUD rectangles. Edits there are dropped. Cheap, heuristic, wrong while the camera is still.
2. A HUD-less copy by draw interception: D3D9 games draw the HUD last with z off and an orthographic projection. A proxy that copies the back buffer before the first such draw (a state heuristic like OptiScaler's HUDfix) gives a HUD-less frame and makes terminal-style compositing possible. More work and per-engine tuning, but exact.

## 5. Automation (Ep2)

- Registry entry: `data\games.json` `hl2ep2` (exe `hl2.exe`, appid 420, the game root as `dir`).
- Command line: `hl2.exe -game ep2 -novid -console -windowed -noborder -w 1920 -h 1080 -condebug +con_logfile lmxxf_console.log +map ep2_outland_01 +exec lmxxf_legacy`, started directly with `SteamAppId=220` in the environment (Steam running), so the rig owns the PID.
- Live control without input: `cfg\lmxxf_legacy.cfg` runs `alias lmxxf_loop "exec lmxxf_phase; wait 60; lmxxf_loop"`. The rig rewrites `lmxxf_phase.cfg`, using `+left` / `-left` for a steady camera pan, `cl_drawhud 0` and `engine_no_focus_sleep 0` (otherwise the game sleeps when it is not in front). Flicker phases run at `fps_max 60` with `cl_yawspeed 30` (about 10 px per frame); fps phases at `fps_max 0`.
- NR settings change live through `lmxxf_nr_settings.trigger` in the dump folder (`Temporal`, `OpticalFlow` 0 none / 1 zero / 2 flow, `Model`, `NetworkSize`, `MultiPass`, `Strength`, `Enabled`). Frame and sequence dumps use the plugin's own triggers.
- Before the phases, a brightness gate takes frame dumps until the feed is no longer black (the map load and its fade-in take about 40 s after launch). Between phases the probe stops at the first device-removed or device-hung line in the proxy or host log.
- Install and restore: copy `d3d9.dll` and `lmxxf_legacy.ini` into `bin\`, not the game root. `shaderapidx9.dll` lives in `bin\` and resolves `d3d9.dll` there first (probe 1 put the proxy in the root and it never loaded). The rig snapshots `ep2\cfg`, `ep2\save` and `hl2\cfg` (hashes) and the whole tree (size and mtime). Afterwards it removes every new file and restores changed guarded files byte-exact.

## 6. Results

### 6.1 Optical flow (offline)

Four recorded Satisfactory sequences, Small 1080, the game's own vectors as truth (`seq_metrics` definitions; flicker is the answer's frame-to-frame change minus the input's, so negative means steadier than the input). `warp` is the mean |I(t) - warp(I(t-1))| with each vector field. The time is the optical flow's GPU time at a 1080 source, functional lease.

| Vectors | flk_static | flk_mc | psnr_mc (dB) | warp | ms |
|---|---|---|---|---|---|
| none (history off) | +0.00053..+0.00069 | +0.00013..+0.00045 | 39.3-42.6 | - | - |
| zero vectors | -0.00090..-0.00014 | +0.00006..+0.00023 | 39.7-42.9 | 0.0056-0.0143 | - |
| **ours** (`OpticalFlow.cpp`) | -0.00044..-0.00009 | -0.00015..-0.00005 | 39.9-43.2 | 0.0040-0.0084 | 0.47-0.57 |
| FidelityFX OF (SDK 1.1, bilinear block upsampling) | -0.00033..+0.00001 | -0.00010..+0.00012 | 39.7-43.0 | 0.0057-0.0127 | 0.12-0.14 |
| game vectors (reference) | -0.00096..-0.00012 | -0.00070..-0.00041 | 40.8-44.0 | 0.0052-0.0107 | - |

- Ours is better than FFX OF on every sequence and quality column. FFX OF was built from source (opticalflow component plus its DX12 backend) and fed the same frames. Its vectors are integer pixels per 8x8 block, which suits frame interpolation but not history reprojection. Nearest and bilinear block upsampling scored the same. Its only advantage is speed (about 0.4 ms less per frame), so it was dropped: no FFX code or dependency remains.
- The still-pixel threshold (2/255 mean luma difference, no search) trades a little motion flicker for static flicker against the version without it. The version with it is the only one kept.
- Game vectors remain well ahead on motion flicker (flk_mc 3-8x lower), although ours warps the frames more closely. Colour-only flow recovers only part of the motion-compensated gain.

### 6.2 Ep2 (probe 4, 1920x1080 windowed, Small, OF on, single card)

| Phase | fps | Notes |
|---|---|---|
| NR bypassed (still / pan, uncapped) | 1177 / 1182 | the proxy present path without the round trip |
| NR on, history + OF, pan, uncapped | 105-109 | every frame answered by the host (0 skipped); host wait 8.0-8.5 ms per frame |
| NR on, 60 fps cap | 53-60 | flicker phases |

| Sequence (12 frames) | flk_static | flk_mc | psnr_mc (dB) | mean motion |
|---|---|---|---|---|
| history + OF, still | -0.00098 | -0.00162 | 44.0 | 0.4 px |
| history off, still | +0.00030 | +0.00006 | 46.4 | 0.2 px |
| history + OF, pan | -0.00097 | +0.00003 | 48.9 | 4.7 px |
| history off, pan | -0.00011 | +0.00104 | 43.3 | 3.3 px |

- Dump gate: feed in [0,1], the answer differs from the feed on 5.6 % of pixels. The scene is dark (feed median 0.0015 linear).
- 0 failed presents and no device-removed or device-hung line in any run. The game folder was restored byte-exact.
- History with colour-only flow does what it did offline. On the pan, motion-compensated flicker drops from +0.00104 to +0.00003 and psnr_mc rises by 5.6 dB. On the still view, flicker goes from above the input's to below it.
- The NR cost caps the game at about 110 fps: the proxy waits on the CPU for each frame's answer, and the host's network plus hand-off takes 5-8 ms at 1080 (Small).
- Timeouts (250 ms) happen only at the first frames (host start, network build) and around frame and sequence dumps and network rebuilds. During each one, frames go to the screen without NR until the host catches up ("skipped").
- Probe 2's fps were wrong. The proxy waited with one auto-reset event for two fences and took any wake-up as the host's answer. After the first timeout it ran free: about 540 presents/s counted as answered while the host ran NR on 110-135 frames/s. Fixed: the wait re-checks the fence value, and the host answers the proxy's frame id.
- Open: the host's frame-id mismatch counter counts every frame in Ep2 (0 in the test title), although the fence values on both sides agree with the frame ids. Most likely the counter itself is wrong; it is not yet explained.

## 7. Multi-GPU (design)

The host already opens its own device on the adapter the proxy names, so the split maps onto the existing ring with one change: the proxy's 9On12 device goes to the render card (bus 7). The host runs on the display card (bus 3).

- A cross-process texture can't be shared across adapters, so the frame crosses as a **cross-adapter shared heap** (`D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER`, row-major, in system memory). The render card copies into it, the display card copies out, and the fences are shared cross-adapter as well. That is the same mechanism as `mgpu/MgpuSwapChain`'s ring, minus the swapchain proxy.
- Presenting: the frame has to reach the screen from the display card. Two ways:
  1. The result comes back over the ring to the render card, and the game's own D3D9 present goes through Windows' cross-adapter present. That is one more PCIe trip and a hybrid present, but it is simple.
  2. The proxy owns a DXGI flip swapchain for the game's HWND on a second D3D12 device on the display card, inside the game process, and the D3D9 present is skipped. The NR'd frame then comes back from the host on the same card, as a same-adapter shared texture. This is the mgpu design and the better one.
- Cost model from the spike rules: about 0.065 ms of render time per MB written to system memory. 1080p BGRA8 is 8.3 MB, so about 0.54 ms per frame on the render card, and about 1.1 ms with option 1's return trip.
- Not built: Ep2 renders hundreds of fps on one card, so the second card only pays off for NR cost, which is already measured for the ring in Satisfactory.

## 8. Effort estimates

| Scope | Work | Estimate |
|---|---|---|
| This prototype to a usable Ep2 mode | Settings UI (no OptiScaler menu in a 32-bit game, so an ini plus an overlay text drawn by the proxy), host crash recovery, resize and fullscreen tests, HUD mask v1 | 3-5 days |
| D3D9 generally (32-bit, on 9On12) | Per-game 9On12 compatibility checks, MSAA resolve instead of forcing it off, `IDirect3DSwapChain9::Present` and `D3DSWAPEFFECT_FLIPEX` paths, gamma/sRGB detection, fallback (c) for titles 9On12 renders wrongly, a launcher-free install | 1-2 weeks, plus about half a day per game for validation |
| 64-bit D3D11 games without upscalers | OptiScaler hooks their DXGI swapchain already. spike/api11vk's `Dx11wDx12SC` gives present callbacks on a D3D12 back buffer. Add OF in-process (the same class) and the existing terminal whole-frame path. No host process. | 3-5 days on top of api11vk |
| 32-bit D3D11 games | The same host split: a 32-bit `dxgi.dll` proxy, D3D11 `CreateSharedHandle` (NT handles) and `ID3D11Fence` (Windows 10 1703+) instead of 9On12 | 1 week |

[hl2-20th]: https://www.tomshardware.com/video-games/pc-gaming/half-life-2-gets-a-major-20th-anniversary-update-and-bundles-lost-coast-with-episodes-the-game-is-now-free-on-steam-until-november-18
[pcgw-hl2]: https://www.pcgamingwiki.com/wiki/Half-Life_2
[vdc-dxvk]: https://developer.valvesoftware.com/wiki/DXVK
[steam-vk]: https://steamcommunity.com/app/220/discussions/0/5221353200109590885/
[9on12-gh]: https://github.com/microsoft/D3D9On12
[interop-spec]: https://microsoft.github.io/DirectX-Specs/d3d/TranslationLayerResourceInterop.html
[interop-blog]: https://devblogs.microsoft.com/directx/coming-to-directx-12-d3d9on12-and-d3d11on12-resource-interop-apis/
[magpie]: https://github.com/Blinue/Magpie
[lmxxf]: https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
[igor]: https://www.igorslab.de/dlss-5-in-star-citizen-ohne-reshade-injektion-so-habe-ich-es-mit-magpie-ausprobiert/
[feeder]: https://sourceforge.net/mirror/dlss-5-feeder/
[immerse]: https://github.com/martymcmodding/iMMERSE
[ffx-of]: https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/
[ffx-lic]: https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/main/docs/license.md
