# mgpu/tools

Diagnostics and capture tools for the multi-GPU work. They build on their own, separately from `OptiScaler.sln`.

**Origin:** carried over from the archived `optinr-amd` project, by the same author, under the MIT licence (`LICENSE` in this folder). The originals were at `tests/optinr_diag`, `tests/sl_tap`, `tests/gpu_pref_probe`, `tests/p2p_probe`, `tools/fetch_*.ps1` and `tools/sl_tap_view.py`. Changes since then:

- New paths: the build output goes to `mgpu/tools/build/`, captures to `data/` at the repository root.
- `sl_tap/run_tap.ps1` no longer sends keystrokes. A run that needs input in the game has to be driven by a person.

| Tool | What it does |
|---|---|
| `optinr_diag` | DXGI adapters matched to HIP devices by LUID, the real PCIe slot link (ADL PMLOG), D3D12 to HIP interop round trip, and cross-adapter shared-heap copy benchmarks (both directions, pipelined). Writes a JSON report to `data/diag/`. |
| `gpu_pref_probe` | Adapter order from `EnumAdapters1` and `EnumAdapterByGpuPreference`, where `D3D12CreateDevice(nullptr)` lands, and output counts per adapter. |
| `p2p_probe/p2p_probe.py` | HIP peer access and host-link bandwidth, using only the driver's `amdhip64_7.dll`. |
| `sl_tap` | `sl_tap.dll` patches the game exe's own import table (Streamline interposer and FidelityFX loader imports, never other modules) and dumps one frame's FG inputs: depth, motion vectors, camera, HUD-less colour and back buffer. `sl_tap_launch.exe` starts the game suspended and loads the DLL with an APC. |
| `scripts/fetch_*.ps1` | Fetch public headers into `third_party/`, which git ignores: HIP (MIT), FidelityFX API (MIT), Streamline (MIT), NGX, and the PresentMon release. |
| `scripts/sl_tap_view.py` | Turns `sl_tap` `.raw` dumps into PNGs. Prints stats, and the UI diff and mask when a back buffer is present. |

## Build

```
powershell -ExecutionPolicy Bypass -File mgpu/tools/build.ps1
```

- Needs VS 2022, CMake and Ninja.
- Configures once, then builds with `-j` set to logical threads minus two.
- `sl_tap` is built only after `scripts/fetch_streamline_headers.ps1` and `scripts/fetch_ffx_headers.ps1` have run.
