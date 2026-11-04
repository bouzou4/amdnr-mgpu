# D3D11 and Vulkan games

What it takes to run the lmxxf_nr plugin, single- and multi-GPU, for games that don't render with D3D12.

## The hub is D3D12

OptiScaler already runs its D3D12 upscalers for D3D11 and Vulkan games through bridges:

| Game API | Bridge | How the game's resources reach D3D12 | Sync |
|---|---|---|---|
| D3D11 | `IFeature_Dx11wDx12` (`*_12` upscalers: `fsr31_12` / FFX = FSR 3.1/4, `xess_12`, `dlss_12`, `fsr2x_12`) | `with_dx12/dx11_with_dx12.cpp`: the game's textures are copied into NT-shared textures, opened on a D3D12 device created on the D3D11 adapter (`WithDx12::PrepareD3D12ForD3D11`) | D3D11 signals a shared fence, the bridge's queue waits; the output is copied back after a D3D12 -> D3D11 fence (`SyncDx12ToDx11`) |
| Vulkan | `IFeature_VkwDx12` (`*OnDx12` / `*_12` upscalers) | D3D12 resources created by OptiScaler and imported into Vulkan (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`); Vulkan copies in and out | Imported D3D12 fences as Vulkan timeline semaphores; the bridge's list is submitted at evaluate time behind a wait the game's `vkQueueSubmit` releases |

In both, the bridge records the upscaler on its own D3D12 list and submits it on its own queue. The plugin's model (record on the evaluate's list, learn the submission through the `ExecuteCommandLists` detour, run `EnqueueHip` behind it on its session queue, consume the answer the next frame) needs nothing else. HIP only ever imports D3D12 resources, so no D3D11 or Vulkan interop with HIP is needed.

## Status per API and placement

| | D3D12 | D3D11 | Vulkan |
|---|---|---|---|
| Before / after upscaling | done | **prototype, works on the harness** (this branch): the bridge fires the upscale callbacks, `gameApi = OptiApi_D3D11`, `OptiUpscale_Bridged` | same change, built on this branch, not run yet: needs a Vulkan title or harness (about 1 day) |
| Terminal (HUD-less, at present) | done | needs the D3D11 -> D3D12 swapchain interop (`Dx11wDx12SC`, upstream, today only built for OptiScaler's FG) on a plugin request, present callbacks in its `Present` on the D3D12 back buffer, and the HUD-less pick from `Hudfix_Dx11` copied into a shared texture: 3-5 days | `vkQueuePresentKHR` hook exists for the overlay; needs a copy of the swapchain image into a D3D12-importable image, NR, copy back, semaphores both ways inside the present: 1-2 weeks |
| Multi-GPU | done | on top of the swapchain interop: the D3D12 swapchain becomes the provider's proxy (ext 6), the D3D11 device goes to the render card through an adapter override in the D3D11 device creation hook (ext 5 extended); the bridge's D3D12 device follows the D3D11 adapter: 1-2 weeks after terminal | no cross-adapter present in Vulkan: the plugin must give the game proxy swapchain images on the render card and present them on the display card through DXGI (a WSI replacement): 3-5 weeks, high risk |

Native D3D11 upscalers (`fsr22`, `xess` on D3D11) have no D3D12 list; callbacks there would need the bridge's resource sharing without its upscaler (2-3 days, low value: FSR 4 needs `fsr31_12` anyway). They get no callbacks.

## The D3D11 prototype

- Host: `IFeature_Dx11wDx12::Evaluate` fills the frame with `PluginApi::FillUpscaleFrame` (moved out of `NVNGX_DLSS_Dx12.cpp`, shared by both paths) with the bridge's device, list and queue and the shared D3D12 views of the game's textures; `colorOverride` is honoured and put back. States follow the same rules as D3D12 (configured barriers, else the NGX convention).
- Plugin: unchanged pipeline; logs and shows `Game API: D3D11, through OptiScaler's D3D12 bridge`.
- Cost: the bridge's copies are paid by the `*_12` upscaler already; NR adds the same work as on D3D12.
- Test: `plugins/lmxxf_nr/tools/d3d11_harness` (a D3D11 app that upscales through OptiScaler's NGX D3D11 exports and presents the output).

Measured (harness, 1280x720 -> 1920x1080, after upscaling, Performance preset = Small 1080 tier with history, 20 s per case):

| Upscaler | Evaluates | NR (GPU network + handoff) | Feed | Edit (dump) | fps |
|---|---|---|---|---|---|
| `fsr31_12` (FSR 4 on the bridge) | 2176 ok, 0 failed | 5.3 ms median, 0 skipped | 0.000-1.000 | mean 0.013, 99.6 % of pixels changed | 120-139 |
| `xess_12` | 2270 ok, 0 failed | 5.2 ms median, 0 skipped | 0.000-1.000 | mean 0.015, 99.9 % | 119-134 |
| `fsr31_12`, plugin not loaded | 16555 ok | - | - | - | 1100-1600 |

The plugin log shows `game API 2, through OptiScaler's D3D12 bridge`, the network built on the bridge's device, and the game's vectors at render size. The edit is smaller than on game frames (flat synthetic bars). The first two runs of the first build crashed (access violation) during the bridge's feature creation, before any callback ran. The rebuilt host passed 3 of 3. The cause is not known; a crash trace is built into the harness.

## Test titles

- D3D11: Baldur's Gate 3 (`bin/bg3_dx11.exe`, ships `libxess_dx11.dll` and `nvngx_dlss.dll`, no anti-cheat); Satisfactory with `-dx11` (ships the NGX D3D11 RHI). Which upscaler a profile has selected must be read from the game's settings before a run.
- Vulkan: Baldur's Gate 3 (`bin/bg3.exe`, the default renderer).
