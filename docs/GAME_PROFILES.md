# Per-game profiles (design)

What a game needs from the plugin differs per title: where the HUD-less colour comes from, what the colour encoding is, which preset fits, and the host quirks. Today it is all hand-set in `[Plugin.lmxxf_nr]`. Profiles make those the defaults for a known exe.

## Storage

- `lmxxf_nr_profiles.ini` beside `lmxxf_nr.asi` (shipped, read-only, versioned with the plugin). One section per exe, matched case-insensitively on the process image name:

  ```ini
  [FactoryGameSteam-Win64-Shipping.exe]
  Name=Satisfactory
  ```
- Optional `Path=` (a substring of the exe's folder) when two titles share an exe name; the longest match wins.
- Resolution order per key: `[Plugin.lmxxf_nr]` in `OptiScaler.ini` (the user) > the profile > built-in default. A profile never overrides a key the user set, so presets and the menu keep working; the menu shows `Profile: <Name>` and marks rows that come from it.
- Not a separate format: the plugin already reads ini keys; a small reader over the file (no new dependency) is enough. An unknown key is logged and ignored, so older plugins read newer files.

## Keys

| Key | Values | What it sets |
|---|---|---|
| `Name` | text | shown in the menu and log |
| `Source` | `hudfix`, `gamefg`, `whole` | terminal NR's HUD-less source; `hudfix` and `gamefg` fall back to the other, then to whole frame after 30 presents when `WholeFrameFallback` is true |
| `WholeFrameFallback` | bool | as today |
| `Placement` | 0 / 1 / 2 | default placement |
| `Preset` / `MgpuPreset` | 1-4 | default preset single-GPU / multi-GPU |
| `UpscaleColour` | `auto`, `sdr`, `scene` | encoding of the colour at the upscale callbacks; `auto` follows `OptiUpscale_Hdr` |
| `Exposure` | `game`, `auto` | scene-linear mapping: the game's exposure texture / pre-exposure, or the smoothed log-average |
| `HdrGuardMedian` | float (1.5) | bypass NR when the mapped median is above it |
| `HdrPaperWhite` | nits (200) | as today, per title when its UI white differs |
| `Quirks` | comma list | `legacy-swapchain` (expects the provider on `IDXGIFactory::CreateSwapChain`), `hdr-cvar` (HDR output needs a game setting, shown as a hint), `menu-cap` (main menu is frame-capped), `no-terminal` (HUD-less known not to work) |
| `Host.<Section>.<Key>` | text | OptiScaler settings the installer writes into `OptiScaler.ini` at install time (e.g. `Host.Upscalers.Dx12Upscaler=ffx`); never applied at run time, so the host stays in charge of its own ini |

## Seeds (from measured runs)

`plugins/lmxxf_nr/profiles/lmxxf_nr_profiles.ini`:

- **Satisfactory** (`FactoryGameSteam-Win64-Shipping.exe`, UE5, D3D12, FSR inputs through the FFX API): HUDfix finds the HUD-less colour once its hooks run with FG off (`Source=hudfix`), terminal placement keeps the UI pixels identical. HDR is PQ (`R10G10B10A2`, colour space 12) only with `r.HDR.EnableHDROutput=1` in Engine.ini (`hdr-cvar`); the main menu is capped by `FG.MaxMainMenuFPS` (`menu-cap`). Presets: Performance on one card (54.6 fps against 76 without NR), Balanced with multi-GPU (Full 1080 with history, 56.2 fps, render card bound).
- **CONTROL** (`CONTROLResonant.exe`, D3D12, FSR 3.1/4 through the FFX API): HUDfix picks nothing; the game hands its FSR FG a HUD-less colour (`R10G10B10A2`, back buffer size), used while its FG is off (`Source=gamefg`); with its FG on NR runs on the whole frame. The colour at the upscale callbacks is scene-linear HDR (median about 1800, peaks 60000): `UpscaleColour=scene`, `Exposure=game`, guard at 1.5 (it trips for a few frames at loading transitions). Creates its swapchain with the legacy `CreateSwapChain` (`legacy-swapchain`). Preset: Performance (Small) multi-GPU, as the play install.

## Later

- Profile keys from the community: a PR to the profiles file, one section per title, with the run that proved it (fps, UI-pixels gate, feed range).
- A `Version=` match (exe file version) when a patch changes a title's behaviour.
