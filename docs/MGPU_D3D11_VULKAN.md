# Multi-GPU NR for D3D11 and Vulkan games (design research)

This is a design and feasibility study; nothing here is implemented. It covers how the D3D12 multi-GPU path (`docs/MGPU_DESIGN.md` 10-11) could carry D3D11 and Vulkan games: the game on the render card (no monitors, bus 7, PCIe Gen4 x8), NR and present on the display card (bus 3, Gen5 x8).

The primary sources are linked inline. "Probe" means a result from `plugins/lmxxf_nr/tools/xapi_probe` on this rig (2x R9700, Windows 11 26200, AMD driver 26.10.1, Vulkan loader 1.4.357). Raw output lives in the git-ignored `data/xapi_probe/`.

## 1. Summary

| | D3D11 | Vulkan |
|---|---|---|
| Feasible | yes | yes |
| Transfer | game back buffer → NT-shared texture (D3D11 copy, VRAM) → D3D12 on the render card → the existing cross-adapter ring | game image → `vkCmdCopyImageToBuffer` straight into system memory that the display card's D3D12 reads (a D3D12 cross-adapter heap imported into Vulkan, or host pages imported on both sides) |
| Sync | D3D11 can open and signal D3D12 cross-adapter fences directly (probe) | Vulkan timeline semaphore imported from a D3D12 cross-adapter fence; the display card's D3D12 sees the signal (probe) |
| Swapchain | OptiScaler's `Dx11wDx12SC` bridge with its D3D12 swapchain made by the plugin's provider (the D3D12 proxy as it is today) | a Vulkan swapchain provider at OptiScaler's `IFGFeature_Vk` swapchain seam: virtual `VkSwapchainKHR` images on the render card; the display card presents through DXGI (the D3D12 presenter as it is today) |
| Device on the render card | adapter override extended to `D3D11CreateDevice` / `D3D11CreateDeviceAndSwapChain` (hooks exist) | filter or reorder `vkEnumeratePhysicalDevices` / groups (hooks exist); Windows' per-app GPU setting also reorders the loader (probe) |
| Display side | unchanged (D3D12, HIP NR, flip + tearing) | unchanged |
| Effort | 2-3 weeks, including the terminal-placement pieces | 3-4 weeks |
| Risk | medium-low | medium (was "high" in `MULTI_API.md`); the transfer and sync primitives are proven, and the open risk is WSI semantics across titles |

Key probe results:

- No D3D11 or D3D12 resource opens on the other card (`E_INVALIDARG`). Microsoft: "There is no mechanism to share cross-adapter resources across D3D API versions" ([shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps)). D3D11 therefore always crosses through D3D12 on its own card.
- D3D11 does open D3D12 cross-adapter fences, made on either card, and its `Signal` is seen on the display card. A D3D11 `SHARED_CROSS_ADAPTER` fence also opens on the display card's D3D12.
- Vulkan on the render card imports a D3D12 `SHARED_CROSS_ADAPTER` heap made on the render card, writes it, and the display card's D3D12 reads it bit-exact. A heap made on the display card does not import (`vkAllocateMemory` returns `VK_ERROR_UNKNOWN`). For Vulkan the ring heap therefore has to be created on the render card, the reverse of today.
- `VK_EXT_external_memory_host` imports the same host pages on both cards (4096-byte alignment). `ID3D12Device3::OpenExistingHeapFromAddress` opens those pages on both cards too. Every combination (Vulkan→Vulkan, Vulkan→D3D12, D3D12→D3D12) reads back bit-exact.
- Both cards report the window's surface as supported, and a Vulkan swapchain on the render card presents 30 of 30 frames (AMD's cross-adapter WSI). D3D11 from the render card does the same (Windows' cross-adapter present). Both are usable as the fallback once the display card is lost.
- Vulkan device groups: 2 groups of 1 device. No mGPU group exists across the two cards.

## 2. What exists to build on

- **The D3D12 path** (`plugins/lmxxf_nr/src/mgpu/`). The adapter override (ext 5) and the swapchain provider (ext 6) feed a proxy `IDXGISwapChain4` whose buffers live on the render card. `MgpuSwapChain` sends each frame over the ring, a placed footprint in a `SHARED | SHARED_CROSS_ADAPTER` heap with fences `filled` / `released`. The display card's presenter thread runs NR and presents (`FLIP_DISCARD`, `ALLOW_TEARING`). The latency spike's `Pacer` (`MgpuLowLatency=1/2`, `spike/latency`) runs off a D3D12 fence clock on the game's queue.
- **OptiScaler's D3D11 swapchain bridge**, `with_dx12/dx11_with_dx12_sc.cpp` (`Dx11wDx12SC`). It is built in `DxgiFactoryHooks::CreateSwapChainForHwnd` / `CreateSwapChain` for D3D11 devices, but today only when OptiScaler's FG is on.
  - It creates the game's real D3D11 swapchain on a hidden window, so the game keeps real D3D11 back buffers.
  - It creates a D3D12 swapchain on the game's window, on the queue of the bridge's D3D12 device (`WithDx12::PrepareD3D12ForD3D11`, same adapter as the D3D11 device).
  - It hands the game a wrapper `IDXGISwapChain4`.
  - At `Present` it does: D3D11 `CopyResource` back buffer → NT-shared copy; D3D11 shared fence `Signal`; D3D12 `Wait`; D3D12 copy shared → D3D12 back buffer; OptiScaler's overlay; D3D12 `Present`.
  - `ResizeBuffers(1)`, `SetFullscreenState`, `GetFullscreenState` and colour space go to the D3D12 swapchain.
  - `PrepareDx12InteropDesc1` turns blt-model / single-buffer descriptions into flip model (minimum 2 buffers, tearing when supported, MSAA refused).
  - The breadth spike measured the same bridge family (`IFeature_Dx11wDx12`) for the upscale callbacks: the D3D11 harness runs FSR 4 + NR through it (`MULTI_API.md`).
- **OptiScaler's Vulkan seams** (`hooks/Vulkan_Hooks.cpp`):
  - `hkvkEnumeratePhysicalDevices` / `...Groups` (they track devices today).
  - `hkvkCreateDevice`, with `IFGFeature_Vk::PrepareDevice` to add extensions and features to the game's device; it already enables timeline semaphores for FG.
  - `hkvkCreateSwapchainKHR` → `IFGFeature_Vk::CreateSwapchain`, and the matching `DestroySwapchain`, `GetSwapchainImages`, `AcquireNextImage`, `QueuePresent` and `SetHdrMetadata` virtuals. This is a complete swapchain-virtualisation interface, used by OptiFG on Vulkan (`FSRFG_Vk`, which replaces the swapchain with FidelityFX's Vulkan frame-interpolation swapchain, and `DLSSG_Vk`).
  - `IsDxgiBackedCaller` / `FromFGOutput` keep DXVK / vkd3d-proton and FG's own swapchains out.
  - `Hudfix_Vk` and `Hudfix_Dx11` exist in this tree. The README's "no D3D11/Vulkan HUDfix" is older than the code.
- **FidelityFX's Vulkan FG swapchain** is the same proxy pattern. `ffxReplaceSwapchainForFrameinterpolationVK` replaces create / destroy / get images / acquire / present / HDR metadata (`FfxSwapchainReplacementFunctions`, FidelityFX SDK `ffx_vk.h`; [GPUOpen](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation-swap-chain/)).

## 3. D3D11

### 3.1 Sharing across adapters (Q1)

| Path | Result | Source |
|---|---|---|
| D3D11 NT-shared texture on R opened by D3D11 on D | `E_INVALIDARG` (probe `d3d11.shared_tex.open_d3d11_other`) | MS documents no cross-adapter resource flag for D3D11 ([D3D11_RESOURCE_MISC_FLAG](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_resource_misc_flag)) |
| the same texture opened by D3D12 on D | `E_INVALIDARG` | "There is no mechanism to share cross-adapter resources across D3D API versions." ([shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps)) |
| the same texture opened by D3D12 on R | OK (what OptiScaler's bridge does) | [OpenSharedHandle](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-opensharedhandle) |
| D3D12 committed cross-adapter **buffer** (to open in D3D11) | `E_INVALIDARG` | committed cross-adapter resources are allowed "only for row-major D3D12_RESOURCE_DIMENSION_TEXTURE2D resources" (shared heaps), and this rig has `CrossAdapterRowMajorTextureSupported = 0`. Placed resources have no shared handle, so D3D11 can never see the ring. A D3D11 compute pack straight into the ring is ruled out. |
| D3D12 cross-adapter fence (made on R or on D) opened by D3D11 on R | OK; D3D11 `Signal(5)` seen by D3D12 on D | [OpenSharedFence](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-opensharedfence) takes handles from "ID3D11Fence::CreateSharedHandle or ID3D12Device::CreateSharedHandle" |
| D3D11 fence with `D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER` opened on D3D12 D | OK; signal seen | [D3D11_FENCE_FLAG](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/ne-d3d11_3-d3d11_fence_flag) |
| D3D11On12 | not tried | A D3D11On12 device runs on its D3D12 device's adapter ([D3D11on12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-11-on-12)). MS: "not optimized for performance ... significant memory overhead ... not recommended ... for complicated 3D scenes". Moving the game's own device onto D3D11On12 would change its performance and is not needed: OptiScaler's NT-handle bridge already reaches D3D12 on the same card. |
| `OpenExistingHeapFromAddress` on both cards (same `VirtualAlloc` pages) | OK; R writes, D reads bit-exact. Heap: `CUSTOM`, write-back, L0, `SHARED_CROSS_ADAPTER` flag set by the runtime (0x421) | [OpenExistingHeapFromAddress](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-openexistingheapfromaddress) ("created in system memory ... wraps the entire VirtualAlloc region"; MS describes it as a diagnostic heap). vkd3d-proton maps it to the same thing ("OpenExistingHeapFrom* maps to D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, which in turn maps to VK_EXT_external_memory_host", `libs/vkd3d/heap.c`). It is a D3D12-only path, so D3D11 still needs the D3D12 hop. |

### 3.2 The cheapest path (Q1)

D3D11 resources can't reach the display card, and no ring memory is visible to D3D11. Every D3D11 path therefore has two hops:

1. On the render card, D3D11 `CopyResource` of the back buffer into an NT-shared texture: VRAM to VRAM, about 33 MB at 4K.
2. D3D12 on the render card copies that texture into the ring slot (placed footprint) on its copy queue. This is today's send, unchanged.

The extra hop can't be skipped by letting the game render straight into a shared texture. A D3D11 flip-model game renders to buffer 0 every frame and keeps its RTV ([IDXGISwapChain::GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getbuffer) remarks: with D3D11 only buffer 0 is writable). A proxy can't rotate the memory behind that texture. Without the VRAM copy, the 2.4 ms system-memory send would have to finish before the game's next frame touches the buffer. The VRAM copy makes the buffer free right away and lets the slow send overlap the next frame, as the D3D12 proxy's rotating buffers do. Cost: see 5.

Sync: D3D11 `Signal` on a fence shared with D3D12 on the same card (what `Dx11wDx12SC` does), then D3D12 `Wait`, the send, and the ring's `filled` cross-adapter fence. Alternatively D3D11 can signal the cross-adapter fence itself (probe), which would help only if a D3D11-side copy wrote the ring, and it can't.

### 3.3 Swapchain redirect (Q1)

Reuse `Dx11wDx12SC` with the D3D12 swapchain made by the plugin:

- **Creation.** In `CreateSwapChainForHwnd` / `CreateSwapChain` for a D3D11 device, build the bridge also when a swapchain provider is registered (today: only with FG input = upscaler and FG output on). Where it calls `FGHooks::CreateSwapChainForHwnd(realFactory, dx12Queue, ...)`, call `PluginApi::ProvideSwapchain(realFactory, dx12Queue, hWnd, &fgDesc, ...)`. `dx12Queue` is the bridge's queue on the render card (the bridge device follows the D3D11 adapter), so the provider gets exactly the D3D12 case it handles today. It returns the `MgpuSwapChain` proxy, buffers on R and real swapchain on D.
- **Present.** `Dx11wDx12SC::Present` already does D3D11 copy → fence → D3D12 copy into the "FG" back buffer → overlay → `Present` on the D3D12 swapchain, which is now the proxy. The proxy's `Present` does the ring send and the display card presents. Two changes:
  - The plugin's present callbacks (ext 2) must fire there, since `WrappedIDXGISwapChain4::LocalPresent` is not in this path: `BeforeOverlay` around `MenuOverlayDx::Present`, `AfterOverlay` before the D3D12 `Present`.
  - The hidden D3D11 swapchain must not be presented under multi-GPU. The bridge presents it every frame (`_real->Present(0)`, kept for Linux overlays). On the render card that is a Windows cross-adapter present (probe: D3D11 presents from R work), so it could add a second 33 MB system-memory copy per frame. It has to be skipped and the render card's copy engine checked in the PoC.
- **One extra D3D12 copy.** The bridge copies the shared texture into the proxy's back buffer, and the proxy then sends that buffer: VRAM → VRAM → system memory. A provider-aware bridge could hand the shared texture to the proxy's send directly, saving a second VRAM copy. That is an optimisation for after the PoC (project rule: optimise last).
- **ResizeBuffers / ResizeBuffers1 / SetFullscreenState / ResizeTarget / colour space / HDR metadata.** The bridge forwards these to the D3D12 swapchain. The proxy already implements them for the display card (README, Robustness: resize with NR held, exclusive fullscreen through the presenter, HDR passthrough, minimise, display-card loss). `GetContainingOutput`, `GetFrameStatistics` and the waitable object come from the proxy.
- **Fallback after a display-card loss.** The proxy's single-GPU fallback is a D3D12 swapchain on the render card. That works for the bridge too; the probe showed Windows' cross-adapter present from R works for D3D11 and D3D12.

What OptiScaler's bridge gives for free: the D3D11 → D3D12 copy and fence plumbing; blt-to-flip conversion; buffer-count and format normalisation; resize, fullscreen and HDR forwarding; the overlay on the D3D12 side; and D3D11 HUDfix's present hooks (`Hudfix_Dx11::PresentStart/End`, `ResTrack_Dx11`). What it lacks: the provider call, the present callbacks, the hidden-present skip, and HUD-less hand-over to the plugin (`Hudfix_Dx11` feeds only OptiScaler's FG today).

### 3.4 Device on the render card (Q1)

- **Adapter override for D3D11.** Call the registered `OptiAdapterOverrideFn` at the top of `hkD3D11CreateDevice` and `hkD3D11CreateDeviceAndSwapChain` (`hooks/D3D11_Hooks.cpp`), with the same caller filter as for D3D12 (DXGI, D3D11, D3D12, OptiScaler and the plugin are left alone).
  - A replaced adapter needs `D3D_DRIVER_TYPE_UNKNOWN` when the game passed `nullptr` + `HARDWARE` (documented `D3D11CreateDevice` rule).
  - The bridge's D3D12 device then follows the D3D11 adapter.
  - `MgpuForwardOutputs` (the render card's `EnumOutputs` returns the display card's outputs) applies unchanged.
- **D3D11CreateDeviceAndSwapChain.** OptiScaler passes it through to the original, which creates the swapchain inside `d3d11.dll`. Whether that reaches the factory hooks, and so the provider, has to be checked in the PoC. If it doesn't, split the call in the hook: `D3D11CreateDevice`, then `IDXGIFactory::CreateSwapChain` through the hooked factory.
- **Windows' per-app GPU setting** (`HKCU\Software\Microsoft\DirectX\UserGpuPreferences`, value = exe path, `SpecificAdapter=1002&7551&54131849;GpuPreference=1073741824;` as Settings writes it; format in optinr-amd's README). Probe with the setting on the probe exe:
  - `EnumAdapters1` and `EnumAdapterByGpuPreference(UNSPECIFIED)` put bus 7 first.
  - `D3D11CreateDevice(nullptr)` / `D3D12CreateDevice(nullptr)` land there (optinr-amd step 4a).
  - The Vulkan loader orders bus 7 first.
  - The **outputs move to bus 7** (Windows' hybrid reporting), and bus 3 reports none.

  That moves games which take the default adapter without any hook. It also breaks "the card with outputs is the display card", so the plugin must identify cards by PCI bus / LUID (`MgpuRenderBus` / `MgpuDisplayBus`), never by output count. It is a user setting, so it stays an optional hint in the docs and the installer never writes it.

## 4. Vulkan

### 4.1 Device groups (Q2)

The spec: "A device group is a set of physical devices that support accessing each other's memory and recording a single command buffer that can be executed on all the physical devices ... they must support identical extensions, features, and properties" (Vulkan spec, Devices and Queues, Device Groups). On Windows, groups need the WDDM linked-adapter mode (Khronos, [Vulkan multi-GPU blog](https://www.khronos.org/blog/vulkan-multi-gpu-support-not-just-for-windows-10)). Probe: 2 groups of 1 device. Groups play no part here.

### 4.2 External memory between Vulkan and D3D12 (Q2)

The spec's compatibility table (Capabilities, `external-memory-handle-types-compatibility`) requires a matching driverUUID and deviceUUID for `OPAQUE_WIN32`, `D3D12_HEAP` and `D3D12_RESOURCE`, and imposes "No restriction" for `HOST_ALLOCATION_BIT_EXT`. Probe (render card):

| Import into Vulkan on R | Result |
|---|---|
| D3D12 `SHARED_CROSS_ADAPTER` heap **made on R**, opened on D | OK. Vulkan writes, D3D12 on D reads bit-exact. Same-device import, so spec-legal; the cross-adapter step is D3D12 ↔ D3D12, which MS supports. The driver's `vkGetPhysicalDeviceExternalBufferProperties` still reports `D3D12_HEAP` as not importable for buffers (`compat 0`): the import works against the capability report. |
| the same heap **made on D** | `VK_ERROR_UNKNOWN` from `vkAllocateMemory` (the spec table requires a matching device) |
| committed cross-adapter buffer (`D3D12_RESOURCE`) | can't be created (committed is textures only, 3.1) |
| host pages (`VK_EXT_external_memory_host`, `VirtualAlloc`) on R **and** on D | OK, memory type 1 (host-visible, coherent) on both. Vulkan R writes; Vulkan D, the CPU and D3D12 D (`OpenExistingHeapFromAddress`) all read bit-exact. The spec allows importing "the same payload into multiple instances of Vulkan" (Memory, Host External Memory). |

Two transfer layouts follow, both a single hop from the game's image to system memory:

- **(a) The ring heap created on the render card's D3D12 device**, opened on the display card, imported into the game's `VkDevice`. The plugin keeps a small D3D12 device on R only for this heap and the fences.
- **(b) Host pages** imported into the game's `VkDevice` and opened on the display card with `OpenExistingHeapFromAddress`. This is spec-clean on the Vulkan side and needs no D3D12 device on R for memory. It is vkd3d-proton's own mapping of cross-adapter heaps.

(a) relies on behaviour the driver doesn't advertise; (b) relies on a "diagnostic" D3D12 heap type. Both pass the probe. Section 5 measures (b) at 2.08 ms per 4K frame. The PoC implements (b) first, with (a) behind a switch.

### 4.3 Semaphores and fences (Q2)

- The spec table requires a matching deviceUUID for `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`.
- Probe: a timeline semaphore imported from a D3D12 `SHARED | SHARED_CROSS_ADAPTER` fence **made on R** (same device, spec-legal) is signalled by a Vulkan submit, and the display card's D3D12 fence reads the value.
- A fence made on D imports and works too, but that one is outside the spec table, so the design uses fences made on R.
- Per frame: the game's present queue waits the game's `pWaitSemaphores`, copies the image to the ring, and signals `filled` (the imported cross-adapter fence). The display card's presenter waits `filled` on the GPU as today.
- `released` (display → render): a D3D12 fence made on R and opened on D. The plugin waits it on the CPU before reusing a slot, or Vulkan waits the imported semaphore.
- No CPU relay is needed. The latency pacer's `RenderClock` watches the same D3D12 fence, made on R and also signalled by the game's frame-end submit.

### 4.4 Presenting on the display card (Q2)

- **DXGI swapchain from the plugin (chosen).** The game's `VkSwapchainKHR` becomes virtual. The existing D3D12 presenter on D (real flip-model DXGI swapchain on the game's HWND, NR, UI composite, tearing) is reused unchanged; only its frame source differs. This keeps Independent Flip, VRR, HDR10 / scRGB (D supports them, R's surface doesn't, below), the waitable-object pacing and the display-card-loss handling.
- **Vulkan swapchain on a second `VkDevice` on D.** Possible (D supports the surface), but it duplicates the presenter in another API and the HIP NR runtime binds to D3D12 resources. It gains nothing.
- **The game presenting itself from R** (AMD's cross-adapter WSI, probe OK). That is Vulkan's M3 equivalent and the loss fallback, not the NR path: no NR, and the copy goes through the driver. The research sweep's `vulkaninfo` shows the headless card offering only SDR surface formats, while the display card offers HDR10 ST 2084 and scRGB. A game asking R for HDR formats would turn HDR off. The provider therefore answers the surface queries (`vkGetPhysicalDeviceSurfaceFormats(2)KHR`, `...CapabilitiesKHR`, `...PresentModesKHR`) with what D's DXGI output supports.

### 4.5 The Vulkan swapchain provider (Q2)

This is plugin API ext 7, sitting at the `IFGFeature_Vk` seam and exclusive with OptiFG on Vulkan as ext 6 is with D3D12 FG. Callbacks:

- **`PrepareDevice`**: enable `VK_KHR_external_memory_win32`, `VK_EXT_external_memory_host`, `VK_KHR_external_semaphore_win32` and timeline semaphores on the game's device (all present on R, probe).
- **`CreateSwapchain`**: create N `VkImage`s on R with the game's format, usage and extent (`TRANSFER_SRC` added). Create the D3D12 proxy's equivalent: the ring and the real DXGI swapchain on D for the HWND (`VkWin32SurfaceCreateInfoKHR::hwnd`, kept from `vkCreateWin32SurfaceKHR`). Colour spaces: `SRGB_NONLINEAR` → DXGI 0, `HDR10_ST2084` → 12, `EXTENDED_SRGB_LINEAR` → scRGB (R16G16B16A16 float).
- **`GetSwapchainImages`**: the virtual images.
- **`AcquireNextImage(2)`**:
  - Return the next free index, the one whose ring send has finished.
  - Signal the game's semaphore and fence with an empty submit.
  - Return `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` when the HWND's client size no longer matches. That is how Vulkan games learn to resize; there is no `ResizeBuffers`.
  - Block here for frame pacing (the Vulkan counterpart of the waitable object).
- **`QueuePresent`**: one submit on the game's queue: wait `pWaitSemaphores`, image → ring copy, signal `filled`. Then schedule the frame for the display presenter, as the D3D12 proxy's `Present` does. Also handle `VkPresentIdKHR` / `VkPresentId2KHR` (R exposes `present_id2` / `present_wait2`; `vkWaitForPresent(2)KHR` is answered from the display presenter's real presents), `VkSwapchainPresentModeInfoEXT` (maintenance1) and multi-swapchain presents (pass through any swapchain that isn't ours).
- **`SetHdrMetadata`**: to `IDXGISwapChain4::SetHDRMetaData` on D.
- **`VK_EXT_full_screen_exclusive`**: application-controlled exclusive mode maps to `SetFullscreenState` on D's swapchain through the presenter (the D3D12 proxy's path). Otherwise borderless with Independent Flip.
- **`DestroySwapchain` / `oldSwapchain`**: retire the old ring after 30 presents, as the D3D12 proxy does on resize.

The probe's 30-of-30 presents from R show the WSI path itself is healthy, but the virtual swapchain is new code. It is the bulk of the Vulkan effort and its main risk: correct semaphore/fence semantics, out-of-date loops, `maxImageCount`, and games that acquire several images ahead.

### 4.6 Interception and device selection (Q2)

- **OptiScaler's hooks rather than an implicit layer.** OptiScaler already hooks the loader (`vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` and the WSI calls) and sits in the game folder as `dxgi.dll` / `winmm.dll`. An implicit layer would need a registry entry under `HKLM`/`HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers` ([LoaderLayerInterface.md](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md)), which is a system install, and it would sit in a chain ordered against the Steam overlay, RTSS, OBS and ReShade (`VK_LAYER_reshade`). A layer would be cleaner for title coverage, but it adds an installer step. It stays an option should OptiScaler's hook order ever lose to another layer.
- **ReShade's add-on API is no route.** Its `create_swapchain` event can rewrite the description (`include/reshade_events.hpp`: "To overwrite the swap chain description, modify desc in the callback and return true"), but it can't replace the swapchain or present elsewhere.
- **Physical device.**
  - The game picks from `vkEnumeratePhysicalDevices` / `vkEnumeratePhysicalDeviceGroups(KHR)`.
  - A plugin callback at those hooks (an "adapter override" for Vulkan) returns the list with the render card first, or only the render card. Hiding the display card is the robust choice: games that score devices see two identical R9700s and may take either.
  - The plugin needs no Vulkan device on D (the display side is D3D12), so hiding it costs nothing.
  - Swapping the physical device inside `vkCreateDevice` is not an option: the game has already read that device's memory types and limits.
- **The loader follows DXGI's order** ([loader_windows.c](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/loader/loader_windows.c) `windows_read_sorted_physical_devices` uses `EnumAdapterByGpuPreference(..., UNSPECIFIED)`). The Windows per-app setting therefore moves default-device Vulkan games without hooks (probe: bus 7 became device 0). `VK_LOADER_DEVICE_ID_FILTER` (loader 1.4.326+, [LoaderInterfaceArchitecture.md](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md)) can't tell two 1002:7551 cards apart. AMD's driver adds its own reordering layer (`VK_LAYER_AMD_switchable_graphics`), which hooks the same enumerations; the PoC checks that OptiScaler's hook sees its output.
- **DXVK / vkd3d-proton.** DXVK presents only through Vulkan WSI and picks devices with `DXVK_FILTER_DEVICE_NAME` / `DXVK_FILTER_DEVICE_UUID` (README). vkd3d-proton implements D3D12 cross-adapter heaps as buffer-only heaps over `VK_EXT_external_memory_host` and reports `CrossAdapterRowMajorTextureSupported = FALSE`, the same layout as layout (b). Neither presents on another adapter. OptiScaler leaves their swapchains to the DXGI side (`IsDxgiBackedCaller`), and the provider must too.
- **Lossless Scaling's dual-GPU mode** is the opposite approach ([guide](https://sageinfinity.github.io/docs/Guides/DualGPUGuide)). The monitor is on the second GPU and the game is moved with the Windows graphics setting. The game presents normally (Windows' cross-adapter copy), and LS captures the window (DXGI duplication / WGC) and generates frames on its GPU. The guide quotes about 3-5 ms added by the PCIe copy, and 4K 240 fps on PCIe 4.0 x8 "accounted for HDR". Its limits: OpenGL isn't moved by the setting, WGC capture "disables independent flip mode globally" ([LS 24H2 note](https://losslessscaling.com/resolution-scale-sgsr-v1-and-windows-11-24h2-compatibility/)), and there is no access to HUD-less colour or motion vectors. Our hooks keep Independent Flip and get the HUD-less colour, so capture isn't a fit, but LS confirms that the Windows setting plus a cross-adapter copy at 4K fits a Gen4 x8 link.

## 5. Transfer cost (Q3)

There is no GPU peer-to-peer on Windows between these cards (`mgpu/tools/optinr_diag`, `MGPU_DESIGN.md` 1). Every path writes system memory once from the render card and reads it once from the display card. No API offers fewer copies:

- A cross-adapter heap is system memory (MS: "only supported in system memory", "D3D12_MEMORY_POOL_L0").
- Host pages imported on both sides are the same thing under another name.
- The only difference between paths is the hops inside the render card's VRAM before the system-memory write.

Bench (`xapi_probe bench`, idle GPUs under the lock queue): 3840x2160 R10G10B10A2 (33.2 MB), GPU timestamps, median of 30. Every row ends with the display card's texture compared byte for byte against the source: 0 mismatched bytes in all rows.

| Path | Render card | Display card ingest |
|---|---|---|
| D3D12 texture → ring (today) | 2.360 ms (14.1 GB/s, 0.071 ms/MB) | 1.160 ms (28.6 GB/s) |
| D3D11 → NT-shared texture (D3D11 `CopyResource`) → D3D12 → ring | 0.060 ms D3D11 copy + 2.358 ms send; CPU wall clock from D3D11 submit to send done 2.74 ms | 1.160 ms |
| D3D12 texture → host pages (`OpenExistingHeapFromAddress` on both cards) | 2.361 ms | 1.160 ms |
| Vulkan image → VRAM buffer (reference) | 0.069 ms | - |
| Vulkan image → host pages (`VK_EXT_external_memory_host` on R, `OpenExistingHeapFromAddress` on D) | **2.078 ms (16.0 GB/s, 0.063 ms/MB)** | 1.160 ms |
| Vulkan image → imported D3D12 cross-adapter heap | not measured: the bench made its heap on D, which doesn't import (4.2). The functional probe with the heap made on R passes; its timing is left for V4 | - |

What the numbers say:

- **D3D11 costs 0.06 ms more than D3D12 per 4K frame.** The VRAM hop is about 2.5 % of the send and runs on the game's D3D11 context. The send itself is today's, at the Gen4 x8 link speed.
- **Vulkan's single hop is 12 % faster than the D3D12 copy-queue send** (2.08 against 2.36 ms). The cause is either the graphics queue's copy path or the host-page target; the D3D12 send into the same kind of host pages is 2.36 ms, which points at the queue. Worth an A/B on the D3D12 path later (send on the direct queue), as an optimisation after the features.
- **The host-page ring is exactly as fast as the `SHARED_CROSS_ADAPTER` heap** (2.361 against 2.360 ms, ingest identical). They are the same system memory.
- **Per-MB cost.** The send is 0.063-0.071 ms per MB on the render card, in line with `SPIKE_RULES.md`'s 0.065 ms/MB of render time. None of these paths changes the PCIe budget. Whatever reduces bytes (UI tiles, half-res HUD-less) applies equally to all three APIs.

## 6. Latency and VRR (Q4)

- **Present.** The display card's real swapchain is the same in all three APIs (flip discard, `ALLOW_TEARING`, `Present(0, ALLOW_TEARING)` for sync 0). PresentMon already shows `Hardware: Independent Flip` for the D3D12 path. MS: "Variable refresh rate displays require tearing to be enabled" and VRR "works on all versions of Direct3D 11 and 12 using DXGI_SWAP_EFFECT_FLIP_* swap effects" ([VRR](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays)). The game's own API never presents, so a D3D11 blt-model game and a Vulkan `FIFO` game both get Independent Flip on D. The game's sync interval / present mode chooses tearing or not, as the D3D12 proxy does today (`IMMEDIATE`/`MAILBOX` → sync 0 + tearing, `FIFO` → sync 1).
- **Pacing (`MgpuLowLatency`, spike/latency).** The `Pacer` needs a D3D12 fence on the render card that completes when the game's frame is done.
  - D3D11: the bridge's D3D12 queue signal after its copy, or the D3D11 shared fence opened in D3D12, which `Dx11wDx12SC` already has.
  - Vulkan: the imported D3D12 fence signalled in the provider's present submit.
  - Mode 1 holds the proxy `Present` / `vkQueuePresentKHR` until the previous frame is done. Mode 2 also delays the return so the next frame's work arrives as the render card frees up. For Vulkan the hold can sit in `vkAcquireNextImageKHR` instead, which most engines already treat as their throttle.
  - Anti-Lag 2 on Vulkan: `VK_AMD_anti_lag` is exposed on R (probe), and OptiScaler's `ll_antilag2` stays on the render card's timeline, as with D3D12.
- **Added latency** should match D3D12 (15-20 ms uncapped, 5 ms at a cap). D3D11 adds the VRAM copy (5); Vulkan adds nothing over D3D12.

## 7. HUD-less sources (Q5)

| | D3D11 | Vulkan |
|---|---|---|
| HUDfix | `Hudfix_Dx11` + `ResTrack_Dx11`. Their present hooks are already in `Dx11wDx12SC::Present`. It feeds OptiScaler's FG only: needs the same "plugin is a consumer" request as D3D12 (ext 2), then the pick is copied into an NT-shared texture next to the back buffer copy. | `Hudfix_Vk` (candidates after the upscale: the upscaler's output, render-pass colour attachments, compute storage images; it already knows the swapchain images). Same consumer request; the pick is copied into the ring next to the image in the provider's present submit. |
| Game's FSR FG inputs | FSR 3.1 FG on D3D11 doesn't exist (FFX FG is D3D12 / Vulkan). | `ffxConfigure` FG `HUDLessColor` through the FFX Vulkan API, if OptiScaler's Vulkan FFX inputs expose it as the D3D12 FFX path does (to check). |
| Fallback | whole frame (`WholeFrameFallback`) | whole frame |
| Motion vectors | upscale callbacks through `IFeature_Dx11wDx12` (breadth spike: works on the harness) | upscale callbacks through `IFeature_VkwDx12` (breadth spike: built, not run) |

## 8. Plan, gates and PoCs (Q6)

Each phase ends with a go/no-go and runs offline first (harnesses, dumps). A game launch is ≤60 s, under the lock queue.

**D3D11** (2-3 weeks):

| Phase | Work | Gate |
|---|---|---|
| D1 (2 d) | Adapter override in `hkD3D11CreateDevice(AndSwapChain)`; `d3d11_harness --mgpu`; Windows' cross-adapter present only | Harness device on bus 7, presents OK; Satisfactory `-dx11` reaches its menu on bus 7 (engine counters) |
| D2 (4 d) | `Dx11wDx12SC` with the provider's proxy; present callbacks from the bridge; hidden D3D11 present skipped | Harness: 10 frames render == display bit-exact; render card copy engine shows one send per frame; PresentMon Independent Flip on D; fps within 5 % of D3D11 native at a cap |
| D3 (3 d) | `Hudfix_Dx11` to the plugin; NR on D; HDR, resize, fullscreen through the bridge | Satisfactory `-dx11` menu: UI pixels unchanged, feed in [0,1], flicker within the D3D12 run's |
| D4 (2 d) | Minimise, display-card loss, 10-minute run, BG3 `bg3_dx11.exe` | No device removal; memory flat; BG3 menu passes the D3 gates |

**Smallest D3D11 PoC:** the existing `d3d11_harness` with OptiScaler + plugin and D1 + D2. Satisfactory `-dx11` is the first game because its D3D12 numbers on the same scene are the baseline.

**Vulkan** (3-4 weeks):

| Phase | Work | Gate |
|---|---|---|
| V1 (2 d) | Vulkan device filter at the enumeration hooks; a minimal `vk_harness` (moving bars, `IMMEDIATE`, frame counter strip) | Harness and Satisfactory `-vulkan` / BG3 Vulkan render on bus 7 and present through AMD's WSI (no NR) |
| V2 (6-7 d) | Vulkan swapchain provider (4.5), transfer layout (b), D3D12-fence sync; display presenter reused | Harness: 10 frames bit-exact; resize via out-of-date; Independent Flip on D; fps within 5 % |
| V3 (4 d) | `Hudfix_Vk` pick to the plugin; MVs through `IFeature_VkwDx12`; HDR10 / scRGB via D's outputs; `VK_EXT_hdr_metadata` | BG3 Vulkan menu: the D3 gates |
| V4 (4 d) | present_id/wait(2), full-screen exclusive, swapchain recreation storms, minimise, loss fallback (native WSI on R), layout (a) A/B | 10-minute run; a second title passes |

**Smallest Vulkan PoC:** `vk_harness` + V1 + V2 with whole-frame NR. That settles whether a virtual swapchain over the ring holds up before any game is involved.

**Titles** (installed; no anti-cheat):

| API | Titles |
|---|---|
| D3D11 | Satisfactory `-dx11` (D3D11RHI ships); Baldur's Gate 3 `bin/bg3_dx11.exe` (DLSS / XeSS DX11); Deep Rock Galactic (UE4, DX11 default) and Subnautica / Valheim (Unity DX11) as extra coverage |
| Vulkan | Satisfactory `-vulkan` (`FactoryGameSteam-VulkanRHI-Win64-Shipping.dll` ships); Baldur's Gate 3 `bin/bg3.exe` (Vulkan default); Valheim `-force-vulkan` (to verify) |

Avoid: CS2, Deadlock (VAC), The Finals, Delta Force, Marvel Rivals, Splitgate, Battlefield (kernel anti-cheat). Red Dead Redemption 2 and No Man's Sky are listed in `games.json`'s roots, but neither is installed.

## 9. Risks

1. **D3D11 hidden-swapchain present** (3.3): it must be skipped, or it doubles the PCIe traffic.
2. **`D3D11CreateDeviceAndSwapChain`** may create its swapchain outside the factory hooks (3.4).
3. **D3D11 MSAA back buffers**: the bridge refuses them, and such games keep single-GPU NR. D3D11 exclusive fullscreen goes through the proxy's existing handling.
4. **The Vulkan virtual swapchain** (4.5): semantics coverage across engines; the largest work item.
5. **Undocumented driver behaviour.**
   - Layout (a) imports a heap type the driver reports as not importable.
   - Layout (b) uses a D3D12 heap MS calls diagnostic.
   - Either could change with a driver. The plugin probes both at start-up (a 64 KB import plus a fence round trip, as `xapi_probe` does) and falls back to single-GPU NR if both fail.
6. **Surface capabilities**: R offers SDR formats only. The provider must answer from D, or HDR games turn HDR off.
7. **The Windows GPU setting moves the outputs to the render card**: identify cards by bus / LUID (3.4).
8. **Layer order** with other Vulkan layers (ReShade, RTSS, the Steam overlay) against OptiScaler's loader hooks: check in V1.

## 10. Probe tool

`plugins/lmxxf_nr/tools/xapi_probe` (`build.ps1`, VS 2022). `xapi_probe.exe func` runs the sharing, import and signal tests on small allocations; `xapi_probe.exe bench` runs the 4K copies of section 5 (needs idle GPUs: take the lock queue). Raw outputs of this study: `data/xapi_probe/{func,func_d3d11_fences,func_gpu_pref,bench}.txt` (git-ignored). Each probe prints one `RESULT` line, and the cards are chosen by outputs, so run it without the per-app GPU setting (3.4).
