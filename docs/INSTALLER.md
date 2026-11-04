# One-click install and uninstall (design)

For end users: put OptiScaler (with the plugin API extensions) and `lmxxf_nr.asi` into a game, point them at a runtime the user already has, and take it all back out byte-exact.

## What ships, what never does

| Ships (release zip) | Never ships |
|---|---|
| `OptiScaler.dll` + its `OptiScaler\` folder (GPL-3.0, source link in the zip), `lmxxf_nr.asi` (MIT), `lmxxf_nr_profiles.ini`, `lmxxf_nr_install.ps1` + a `.cmd` launcher | the NR runtime build, HIP modules, any weights; NVIDIA, NeuralHub or other third-party binaries |

The runtime and weights stay where the user put them. The installer asks for that folder once (or finds `DLSS5-AMD\` in the usual places: the game folder, a previous install, a path the user typed), checks the layout the plugin understands (`LmxxfNrRuntime.dll`; `native-game-tiled-assets\HIP\SHA256SUMS` and that each listed module hashes right) and writes `RuntimeDir` / `ModulesDir` / `WeightsDir` into the ini. Nothing is copied, so nothing of it is redistributed and an uninstall can't touch it.

## Tool

A PowerShell script (no installer framework, readable by the user), run from a `.cmd` so double-click works:

1. **Pick the game.** Steam libraries from `libraryfolders.vdf` and `appmanifest_*.acf`; other stores by "browse to the exe". Match the exe against the profiles file; unknown exes are allowed with a warning. Refuse games with known anti-cheat folders (`EasyAntiCheat`, `BattlEye`) unless forced.
2. **Checks.** Game not running. The exe folder is the one the game loads proxies from (UE: `<Game>\Binaries\Win64`). Existing proxies (`dxgi.dll`, `winmm.dll`, `version.dll`, `d3d12.dll`): an existing OptiScaler is upgraded in place (its ini kept and merged), anything else (ReShade, Special K) makes the tool pick another proxy name OptiScaler supports or stop.
3. **Snapshot.** Before writing, for every path it will create or replace: a copy of the existing file and its SHA-256, or "absent". Written to `%LOCALAPPDATA%\lmxxf_nr\installs\<id>\manifest.json` (outside the game folder, so Steam's verify can't remove it), `id` = store + app id or a hash of the exe path.
4. **Install.** Proxy dll, `OptiScaler\` folder, `OptiScaler\plugins\lmxxf_nr.asi`, the profiles file, and the ini: OptiScaler's default ini (or the existing one) with `LoadAsiPlugins=true`, the profile's `Host.*` keys, and `[Plugin.lmxxf_nr]` with the runtime paths and, when two GPUs are found, `MultiGpu` as the user chose. The SHA-256 of each written file goes into the manifest.
5. **Uninstall.** Game not running. For every manifest entry: if the file now differs from what was written (the user edited the ini, OptiScaler saved its menu), keep that version beside the manifest as `*.user` and say so; then put the original back (or remove the file when it was absent) and verify the hash. Files that appeared under the game folder after the install and are not ours (OptiScaler logs, `lmxxf_nr_dumps\`) are moved into the install folder under `%LOCALAPPDATA%`, never deleted. Directories the install created are removed only when empty. Last step: every manifest path compared against the snapshot; any mismatch is printed and the manifest kept.
6. **Repair / update.** Uninstall then install with the same choices; a game update that replaced a file of ours is detected by hash and reported (the update wins, the backup of that path is dropped).

## Why not

- **Copying the runtime into the game:** redistribution risk, and several games would each get a copy to keep in sync.
- **An MSI / Inno installer:** opaque to users who want to see what touches their game, and a signing problem anyway. A signed small exe can wrap the same steps later.
- **Steam launch options (`WINEDLLOVERRIDES`, `-dx11`)**: Linux / per-title; the profile can tell the user, the tool doesn't write Steam's config.

## Prior art in this repository's rigs

The CONTROL play install / revert and the Satisfactory probe use the same pieces (hash manifest, byte-exact restore, new files moved aside, empty-directory cleanup) and have restored those games byte-exact across many runs.
