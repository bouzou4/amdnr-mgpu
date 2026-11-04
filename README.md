# OptiScaler plugin API extensions and the lmxxf Neural Rendering plugin

This branch (`mgpu/plugin`) is plain upstream OptiScaler plus a small set of plugin-API extensions, and an ASI plugin that runs lmxxf's DLSS 5 Neural Rendering runtime through those extensions.

| Path | What |
|---|---|
| `src/` | Upstream OptiScaler (GPL-3.0) as a git subtree, plus the plugin-API extensions (`src/plugin_sdk/optiscaler_plugin.h`, `src/OptiScaler/plugins/`). Each extension is its own commit. |
| `plugins/lmxxf_nr/` | The plugin (MIT). It loads a user-installed lmxxf runtime through lmxxf's MIT C API (`LmxxfNrApi.h`). No runtime, weights or kernels are part of this repository. |
| `docs/PLUGIN_API_EXTENSIONS.md` | One section per extension: rationale, ABI, versioning. Written to become the upstream PR descriptions. |
| `docs/PLUGIN_API_ASSESSMENT.md` | The assessment of the stock plugin API these extensions answer. |
| `docs/MGPU_DESIGN.md` | The multi-GPU design the plugin's multi-GPU mode follows. |
| `third_party/directx_agility_sdk/` | DirectX Agility SDK 1.615 headers (MIT), first on the include path so upstream builds with Windows SDK 10.0.22621. |
| `mgpu/` | Build script and diagnostics (MIT). |

Build: `mgpu/scripts/build.ps1` (OptiScaler, Release x64, VS 2022 v143) and `plugins/lmxxf_nr/build.ps1` (the plugin). Both use logical threads - 2.

Credits: OptiScaler by cdozdil and contributors. lmxxf's DLSS 5 on AMD runtime by Kien (MIT).
