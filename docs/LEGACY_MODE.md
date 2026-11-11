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

### 4.1 Controls (hotkeys and menu)

There is no OptiScaler menu in a 32-bit game, so the proxy draws its own: Dear ImGui (OptiScaler's vendored copy under `src/OptiScaler/include/imgui`, DX12 backend, stb_truetype instead of FreeType) on the unwrapped back buffer, recorded on the proxy's D3D12 queue after the host's answer is copied back and before the buffer goes back to 9On12.

| Key (default) | Action | ini key (`[Legacy]`, VK code) |
|---|---|---|
| Insert | menu open / closed (Esc also closes it) | `KeyMenu` |
| End | NR on / off (frames skip the host) | `KeyNr` |
| Page Up | network size 1080 tier -> 1440 -> native (live; the network rebuilds for a few seconds) | `KeySize` |
| Page Down | network history on / off | `KeyHistory` |
| Home | optical flow on / off (off: zero vectors, static history) | `KeyFlow` |

- The keys are unbound in HL2's defaults, and no modifiers are used because the player's config binds Ctrl and Alt. A window subclass on the device window handles them, and the game never sees them. Each press shows a 2 s toast and logs a line. `Hotkeys=0` / `Overlay=0` turn them off, `StartupHint=0` drops the start-up toast.
- The menu has preset, NR, network size, model, passes, style, strength, colour, guided edit upsample, stabiliser, history and motion source.
  - Live readouts: fps, host NR / network / optical flow ms, the proxy's wait, network resolution, frames with / without NR, bypassed frames, host timeouts, skipped frames, failed presents and the flicker meter.
  - Changes go to the host through `Legacy::Control` in the shared block (version 2, layout asserted in both builds); the host applies them when its serial changes. They are saved to `lmxxf_legacy.ini` 0.5 s after the last change.
- Input while the menu is open: key presses, characters, mouse buttons, wheel and `WM_INPUT` are kept from the game. Releases pass so that a key held when the menu opened does not stick. The cursor is drawn by ImGui and moves with raw mouse deltas (the game keeps the OS cursor captured), or with client positions when no raw input comes. Keyboard navigation works too.
- Tests without input: `proxy_keys.trigger` in the dump folder (action names `menu`, `nr`, `size`, `history`, `flow`, one per line) runs the same actions. `proxy_dump.trigger` writes the presented back buffer, overlay included, to `proxy_frame.bmp`. The test title takes a key script (`ms:vk,...`) and posts the keys to its own window.
- The lmxxf runtime polls `GetAsyncKeyState` itself: F9 cycles `DLSS5_MULTI_PASS` and rewrites its `custom-config.txt`, and F8 toggles ViT reuse. HL2 binds F9 to quick load. `Runtime::Load` turns both off (`DLSS5_MULTI_PASS_HOTKEY=0`, `DLSS5_VIT_REUSE_HOTKEY=0`). F7 (the runtime's text overlay) has no switch.
- Both logs open with `FILE_SHARE_READ`, so they can be tailed while the game runs.
- With `MultiGpu=1` the menu draws on the display card's swapchain (section 7).

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
- Frame-id mismatches (fixed after probe 4): the host counted one per frame in Ep2 and none in the test title. The host had the same stale-event bug as probe 2's proxy. Its fence wait (100 ms poll) took any wake-up of its auto-reset event as the fence value. Ep2 has present-free gaps (the 15-30 s map load, bypass phases), so the host's waits time out and register again. When the value finally arrives, the extra registration leaves the event set. The next frame's wait then returns at once, before the proxy has signalled. The host read the previous frame id, counted a mismatch and polled again. That iteration's registration left the event set again, so it happened once per frame from then on. Answers were never wrong: the host skips ids it has already answered, and its GPU queue waits on the fence. The test title has no gap once the host is ready, so it never hit the bug. Fixed: the host re-checks the fence value after each wait, as the proxy does.
  - Offline repro: the test title's new pause argument stops presenting for a while (one gap at a third of the run). With a 2 s gap, the old host counts 0 before the gap and 1017 at frame 1594. With the fix it counts 0 with 2 s and 5 s gaps.
  - Ep2 probe 5 (the same phases, single card): 0 mismatches through frame 2846 (probe 4: 3841 at frame 3842). There are 0 failed presents, no device-hung line, and the folder is clean. The host ran NR on every frame it received except the 3 startup frames, as in probe 4. Timeouts: 15 (probe 4: 14), at the same places (start, dumps, network rebuilds).
  - Flicker, probe 5 against probe 4. History + OF still: flk_static -0.00121 against -0.00098, flk_mc -0.00144 against -0.00162. History + OF pan: flk_static -0.00094 against -0.00097, flk_mc +0.00005 against +0.00003, psnr_mc 48.95 against 48.94. The history-off sequences differ because the scene differs: the load took 33 s instead of 16 s, so the camera ended up elsewhere. History-off pan has 0.41 moving against 0.31, flk_mc +0.00076. History-off still has no moving pixels, so flk_mc is undefined. The fix only changes the host's CPU wait, not the NR path.
  - nr_pan was at 82 fps against probe 4's 105-109, with host GPU time 6.3 ms against 5.0 ms. Other agents' CPU and memory load was high during the run. The fix doesn't touch GPU work, so this needs a re-time on a quiet machine before it counts.

### 6.3 Crysis 2 Maximum Edition (Steam 108800, 2560x1440 fullscreen, Full, 1440, 2 passes, OF on, single card)

This is the second game, and it needed no proxy changes. It ran the UI build and runtime `stage\current`, the same as the play install.

- The game is 32-bit CryEngine 3. `bin32\Crysis2.exe` is SteamStub-wrapped. Starting it directly with `SteamAppId=108800` in the environment works, and so does `steam://rungameid/108800`. `CryRenderD3D9.dll` loads `d3d9.dll` with `LoadLibrary`, so the proxy goes into `bin32\` next to the exe. The game calls `Direct3DCreate9` (not Ex) twice and `CreateDevice` once per adapter (1x1 probes), then creates the real device.
- DX9 selection: the default is DX11. `r_Driver = "DX9"` in the game root's `system.cfg` has no effect, because the player's `Saved Games\Crysis2\game.cfg` sets `r_Driver = "DX11"`. Either `-dx9` on the command line or `r_Driver = "DX9"` in that `game.cfg` selects `CryRenderD3D9.dll`; both were verified. In DX11 mode the proxy still loads, because the DX11 renderer also calls `Direct3DCreate9`, but no device is created and nothing is processed.
- The game boots to a title screen ("press Enter to begin"), so no level is needed. Keyboard input goes through DirectInput, so a key posted to the window does not move past it. The title animates slowly, so it is a near-still scene.
- The DRM components (`bin32\EACore`) raised no prompt and no online check before the title screen.

| Phase | fps | Notes |
|---|---|---|
| NR bypassed (title, uncapped) | 431 | |
| NR on, steady (no dumps) | 28.3-29.8 | host GPU 29.1 ms (network 28.9 ms, OF 0.04-0.2 ms), host wait 32-34 ms per frame |

| Sequence (12 frames, title) | flk_static | flk_mc | psnr_mc (dB) |
|---|---|---|---|
| history + OF | -0.00028 | -0.00008 | 32.5 |
| history off | +0.00063 | +0.00123 | 36.4 |

- Dump gate: the feed is in [0,1]. The answer differs from the feed by 0.018 on average, and by more than 1/255 on 6.5 % of pixels. The title is dark (feed median 0.0013 linear).
- 0 failed presents and no device-removed or device-hung line. The overlay menu draws over the game (Insert, or `menu` in `proxy_keys.trigger`).
- Timeouts (250 ms) happen at the first frames and around every frame or sequence dump. At 2560x1440 the host is still busy with a dump when the proxy's wait ends. After a timeout the proxy presents raw frames until the host catches up. Between dumps that was 250-650 raw frames per 4 s window, and fps reads 80-180 there. The steady phases and the play-install launch had no timeouts.
- Automation: `data\legacy\crysis2_probe.ps1` runs with `-dx9`, a brightness gate, and bypass / NR / menu / history-off phases. `c2_sample.ps1` samples the threads of a stuck process. The probe restores `system.cfg` and `Saved Games\Crysis2` byte-exact (the game rewrites `Profiles\default\*.xml` at every start) and removes `Game.log` and other new files from the game folder.

## 7. Multi-GPU (`MultiGpu=1`)

The game renders on the render card (bus 7, no outputs). NR and presenting happen on the display card (bus 3). The host is unchanged: it opens the adapter whose LUID the proxy writes into the block, which is now the display card's.

```
 32-bit game process                                                         x64 host (display card)
 game thread:  9On12 on the render card's D3D12 device
   Present hook: back buffer -> ring slot m % 3 (cross-adapter heap, system memory)
                 Signal send fence m (cross-adapter) ; the real Present is skipped
 presenter thread (display card device + DXGI flip swapchain on the game's window):
   CPU wait send fence (newest m: older frames are skipped)
   slot -> shared frame texture ; Signal ingest m (the game may refill the slot)
   Signal 2n-1 ------------------------------------------------------------> OF + NrEngine::RunOwn
   CPU wait 2n (250 ms) <---------------------------------------------------- Signal 2n
   frame texture -> swapchain back buffer, overlay, Present
```

- **9On12 on a card without outputs.** D3D9 adapters are outputs, so every one is on the display card. d3d9.dll and 9On12 (`Adapter::Adapter` in the 9On12 source) both take the app's D3D12 device only if its LUID is the adapter's. Otherwise d3d9.dll silently creates a native D3D9 device on the display card (QI for `IDirect3DDevice9On12` fails). The proxy therefore patches `ID3D12Device::GetAdapterLuid` (vtable slot 43) so that the render device reports the display card's LUID, but only on the thread and only while `Direct3DCreate9On12(Ex)` and `CreateDevice(Ex)` run. Outside those calls the real LUID comes back (self-checked at start-up). Swapping the runtime's KMT adapter for the render card's instead made `CreateDeviceEx` fail with D3DERR_NOTAVAILABLE.
- **Render card choice:** the adapter on a PCI bus (D3DKMT adapter address) without outputs, other than the display card. `MgpuRenderBus` forces a bus. DXGI lists two extra R9700 entries with no bus; they are skipped.
- **Ring:** 3 row-major buffers in one `D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER` heap, created on the display card and opened on the render card. The send fence is created shared cross-adapter on the render card. The game waits on the CPU, never longer than `TimeoutMs`, until the presenter has ingested frame m - `MgpuAhead` (default 1), so it renders frame N+1 while the host runs NR on N. A frame whose slot is not free in time is dropped (`dropped`). A resize recreates the ring; the old one is released once both queues have passed it.
- **Waits (the main plugin's lessons, data\hang\ROOTCAUSE.md):** the presenter waits for the send on the CPU before anything of the frame is queued on the display card. The display card's queues hold no wait on a render-card fence. The host's GPU wait is on a display-card fence that the presenter's queue signals, and it has already completed when queued, because the host waits on the CPU first. Every CPU wait has a timeout. The presenter stops (and the game's own present takes over, without NR) at the first device-removed or failed Present (`DeviceLost`).
- **Presenting:** a flip-discard swapchain (3 buffers, tearing when the game asked for an immediate interval) on the game's window, created by the presenter thread on the display card's queue. A minimised or occluded window gets test presents only.
- **Overlay:** the menu and toasts draw on the swapchain back buffer on the display card. `proxy_dump.trigger` dumps that buffer. The menu shows the game's fps, latency, dropped and hidden frames.
- **Fullscreen.** 9On12 runs the device windowed, because the render card has no output to own. The game's own `D3DPRESENT_PARAMETERS` keep its fullscreen request: the runtime gets a copy, and the game's struct gets the results back. When the game read back `Windowed=TRUE`, Crysis 2 switched itself to windowed.
  - `MgpuFullscreen=1` (default): the window covers its monitor, and DWM scales the game-sized swapchain.
  - `0`: the window stays as the game made it.
  - `2`: also switches the display mode (CDS_FULLSCREEN) in the CreateDevice/Reset hook. The windowed 9On12 device then reports D3DERR_DEVICELOST and never recovers: Crysis 2 stalls in its lost-device loop.
  - `3`: DXGI exclusive fullscreen (`SetFullscreenState`). Crysis 2 pauses the same way.
  - While the proxy moves the window or changes the mode, the game's window proc does not see the resulting messages (`g_shieldUntil`).
- Keys (`[Legacy]`): `MultiGpu`, `MgpuAhead` (1-2), `MgpuFullscreen` (0-3), `MgpuRenderBus`, `MgpuSyncInterval` (-1 = the game's interval).
- Latency below is the time from the game's present call to the real Present of that frame. Single-GPU, that is the host round trip; multi-GPU, it is the ring wait plus NR plus the present.

### 7.1 Results (2026-11-10, bus 7 at PCIe **2.0** x8: functional, not a perf baseline)

Bus 7 came up as Gen2 (warm boot), so a send takes 2-8 ms instead of about 0.5 ms. Network Full, 1440, 2 passes, history + OF, runtime `stage\current`. The NR rows are "steady" 4 s windows: no timeout, every shown frame answered by the host. Dumps cause timeouts, and after a timeout raw frames are mixed in until the host catches up, in both modes.

| Case | fps single / multi | latency ms single / multi | host GPU ms | failed presents | device errors |
|---|---|---|---|---|---|
| Test title, 1920x1080 windowed | 49.2 / 49.0 | 20.0 / 40.4 | 17.6 | 0 / 0 | none |
| Ep2 `ep2_outland_01`, 1920x1080 windowed | 47.2 / 49.4 | 20.4 / 40.1 | 17.4-17.8 | 0 / 0 | none |
| Crysis 2 title screen, 2560x1440 (multi: `MgpuFullscreen=0`) | 28.6 / 29.5 | 34.5 / 67.1 | 29.6-30.4 | 0 / 0 | none |
| **Crysis 2 in a level** (`Downtown`, standing), 2560x1440 fullscreen (multi: `MgpuFullscreen=1`) | **24.1 / 29.4** | 36.3 / 67.4 | 29.1-29.6 | 0 / 0 | none |
| Crysis 2 in the level, NR bypassed | 174 / 64-128 | - | - | 0 / 0 | none |

- Where the game's own rendering costs time (the Crysis 2 level: about 5.7 ms per frame), the second card gives +22 % here (+48 % in the 7.2 launch). Single-GPU, render and NR add up on one card. Multi-GPU, NR alone sets the frame time.
- Where the game is nearly free (test title, Ep2, Crysis 2 title), NR sets the frame time on both, and the second card only adds one frame of latency.
- The multi-GPU ceiling is the host round trip, 33.6 ms against 29.5 ms of NR GPU time. The presenter is serial: it ingests frame N+1 only after N is presented, and the host idles meanwhile. Pipelining that would bring it to about 33 fps.
- Bypassed, the presenter waits 7-8 ms for each 14.4 MB 1440p send at Gen2. Section 7.2 has the re-time at Gen5.
- Crysis 2 levels load from the command line: `-dx9 -devmode +map Downtown`. The run creates `Saved Games\Crysis2\SaveGames\default_downtown_*`, and the probe removes it.
- Quality is unchanged: the same host, the same network, the same OF.

### 7.2 Re-time at PCIe 5.0 (2026-11-10 20:03-20:27, perf leases)

Both cards read PCIe 5.0 x8 before the runs. Same settings and builds as 7.1, one launch per case.

| Case | fps single / multi | latency ms single / multi | send wait ms (multi) | failed presents | device errors |
|---|---|---|---|---|---|
| Test title, 1920x1080 windowed | 49.2 / 48.9 | 20.1 / 40.5 | under 0.4 | 0 / 0 | none |
| Ep2 `ep2_outland_01`, 1920x1080 windowed | 46.5 / 48.8 | 20.5 / 40.6 | under 0.9 | 0 / 0 | none |
| Crysis 2 in a level (`Downtown`), 2560x1440 fullscreen | 19.4 / 28.7 | 38.9 / 68.9 | 0.0 (NR on) | 0 / 0 | none |

- The link speed does not change the NR-on numbers. Multi-GPU is bound by the host round trip (34.3 ms per frame in Crysis 2), and the game waits for a ring slot (24-28 ms), not for the send.
- Crysis 2 single-GPU varies between launches: 19.4 fps here against 24.1 in 7.1. NR bypassed, the same two launches ran 105 and 174 fps, so the scene cost differed. Multi-GPU was 28.7 and 29.4. The gain is therefore between +22 % and +48 %, from one launch per case.
- Open: with NR bypassed in the level, multi-GPU shows 64 fps against 105 single, with the send at about 4 ms. What costs the rest is not found (candidates: the render-card copy on the game's direct queue, the skipped 9On12 Present). It does not limit the NR-on case.

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
