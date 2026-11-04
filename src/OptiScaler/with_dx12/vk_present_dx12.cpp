#include "pch.h"
#include "vk_present_dx12.h"

#include <with_dx12/with_dx12.h>
#include <menu/menu_overlay_vk.h>
#include <plugins/PluginApi.h>
#include <State.h>
#include <Config.h>
#include <SysUtils.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <mutex>
#include <vector>

namespace
{
constexpr uint32_t kSlots = 8;

struct Slot
{
    VkCommandBuffer toDx12 = VK_NULL_HANDLE;
    VkCommandBuffer back = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
};

struct Shared
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ID3D12Resource* res = nullptr;
};

struct Bridge
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent {};
    std::vector<VkImage> images;
    bool usable = false;

    // created on the first present
    bool ready = false;
    bool failed = false;
    uint32_t family = UINT32_MAX;
    Shared frame;
    Shared hudless;
    uint64_t hudlessFrame = 0; // present frameId the HUD-less copy belongs to
    ID3D12Device* d3d = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t value = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    Slot slots[kSlots] {};
    uint64_t frames = 0;
};

Bridge _b;
std::mutex _mutex;

DXGI_FORMAT ToDxgi(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

uint32_t ToDxgiColorSpace(VkColorSpaceKHR cs)
{
    switch (cs)
    {
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
        return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
        return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    default:
        return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
}

void Release()
{
    auto& b = _b;
    if (b.device != VK_NULL_HANDLE && b.ready)
        vkDeviceWaitIdle(b.device);
    if (b.queue != nullptr && b.fence != nullptr)
    {
        // drain the D3D12 side before its resources go
        HANDLE e = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (e != nullptr && b.queue->Signal(b.fence, ++b.value) == S_OK &&
            b.fence->SetEventOnCompletion(b.value, e) == S_OK)
            WaitForSingleObject(e, 2000);
        if (e != nullptr)
            CloseHandle(e);
    }
    for (auto& s : b.slots)
    {
        if (s.fence != VK_NULL_HANDLE)
            vkDestroyFence(b.device, s.fence, nullptr);
        if (s.done != VK_NULL_HANDLE)
            vkDestroySemaphore(b.device, s.done, nullptr);
        s = {};
    }
    if (b.pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(b.device, b.pool, nullptr);
    if (b.timeline != VK_NULL_HANDLE)
        vkDestroySemaphore(b.device, b.timeline, nullptr);
    for (auto* sh : { &b.frame, &b.hudless })
    {
        if (sh->image != VK_NULL_HANDLE)
            vkDestroyImage(b.device, sh->image, nullptr);
        if (sh->memory != VK_NULL_HANDLE)
            vkFreeMemory(b.device, sh->memory, nullptr);
        if (sh->res != nullptr)
            sh->res->Release();
        *sh = {};
    }
    b.hudlessFrame = 0;
    if (b.fence != nullptr)
        b.fence->Release();
    if (b.queue != nullptr)
        b.queue->Release();
    b.pool = VK_NULL_HANDLE;
    b.timeline = VK_NULL_HANDLE;
    b.fence = nullptr;
    b.queue = nullptr;
    b.d3d = nullptr;
    b.ready = false;
    b.value = 0;
}

bool Fail(const char* what)
{
    LOG_ERROR("Vulkan present bridge: {}, present callbacks off", what);
    _b.failed = true;
    Release();
    return false;
}

bool CreateD3D12()
{
    auto& b = _b;

    VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
    {
        ScopedSkipSpoofingGlobal skip {};
        vkGetPhysicalDeviceProperties2(b.pd, &props);
    }
    if (!id.deviceLUIDValid)
        return Fail("no LUID for the Vulkan device");

    LUID luid;
    memcpy(&luid, id.deviceLUID, sizeof(luid));

    IDXGIAdapter* adapter = nullptr;
    {
        ScopedSkipSpoofingGlobal skip {};
        IDXGIFactory4* factory = nullptr;
        if (CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)) == S_OK)
        {
            factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
            factory->Release();
        }
    }

    b.d3d = WithDx12::RequestD3D12Device(D3D_FEATURE_LEVEL_12_0, adapter);
    if (adapter != nullptr)
        adapter->Release();
    if (b.d3d == nullptr)
        return Fail("no D3D12 device");

    const LUID dl = b.d3d->GetAdapterLuid();
    if (dl.LowPart != luid.LowPart || dl.HighPart != luid.HighPart)
        return Fail("the D3D12 device is on another adapter than the Vulkan device");

    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (b.d3d->CreateCommandQueue(&qd, IID_PPV_ARGS(&b.queue)) != S_OK)
        return Fail("CreateCommandQueue");
    b.queue->SetName(L"OptiScaler Vulkan present bridge");

    if (b.d3d->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&b.fence)) != S_OK)
        return Fail("CreateFence");
    return true;
}

bool MakeShared(Shared& out, const wchar_t* name, DXGI_FORMAT dxgi, PFN_vkGetMemoryWin32HandlePropertiesKHR getMemProps)
{
    auto& b = _b;
    // The shared image (D3D12 owns the memory)
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = b.extent.width;
    rd.Height = b.extent.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = dxgi;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (b.d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       IID_PPV_ARGS(&out.res)) != S_OK)
        return Fail("CreateCommittedResource");
    out.res->SetName(name);

    HANDLE h = nullptr;
    if (b.d3d->CreateSharedHandle(out.res, nullptr, GENERIC_ALL, nullptr, &h) != S_OK)
        return Fail("CreateSharedHandle (image)");

    VkExternalMemoryImageCreateInfo ext { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkImageCreateInfo ici { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = b.format;
    ici.extent = { b.extent.width, b.extent.height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(b.device, &ici, nullptr, &out.image) != VK_SUCCESS)
    {
        CloseHandle(h);
        return Fail("vkCreateImage");
    }

    VkMemoryRequirements req {};
    vkGetImageMemoryRequirements(b.device, out.image, &req);
    VkMemoryWin32HandlePropertiesKHR hprops { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
    if (getMemProps(b.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, h, &hprops) != VK_SUCCESS)
    {
        CloseHandle(h);
        return Fail("vkGetMemoryWin32HandlePropertiesKHR");
    }

    VkPhysicalDeviceMemoryProperties mp {};
    vkGetPhysicalDeviceMemoryProperties(b.pd, &mp);
    uint32_t type = UINT32_MAX;
    const uint32_t bits = req.memoryTypeBits & hprops.memoryTypeBits;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    if (type == UINT32_MAX)
    {
        CloseHandle(h);
        return Fail("no memory type for the shared image");
    }

    VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = out.image;
    VkImportMemoryWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated };
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    import.handle = h;
    VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    const bool memOk = vkAllocateMemory(b.device, &mai, nullptr, &out.memory) == VK_SUCCESS;
    CloseHandle(h);
    if (!memOk)
        return Fail("vkAllocateMemory (import)");
    if (vkBindImageMemory(b.device, out.image, out.memory, 0) != VK_SUCCESS)
        return Fail("vkBindImageMemory");
    return true;
}

bool CreateShared()
{
    auto& b = _b;
    auto getMemProps = (PFN_vkGetMemoryWin32HandlePropertiesKHR) vkGetDeviceProcAddr(
        b.device, "vkGetMemoryWin32HandlePropertiesKHR");
    auto importSem =
        (PFN_vkImportSemaphoreWin32HandleKHR) vkGetDeviceProcAddr(b.device, "vkImportSemaphoreWin32HandleKHR");
    if (getMemProps == nullptr || importSem == nullptr)
        return Fail("external memory / semaphore functions missing");

    const DXGI_FORMAT dxgi = ToDxgi(b.format);
    if (dxgi == DXGI_FORMAT_UNKNOWN)
        return Fail("unsupported swapchain format");

    if (!MakeShared(b.frame, L"OptiScaler Vulkan present image", dxgi, getMemProps) ||
        !MakeShared(b.hudless, L"OptiScaler Vulkan present HUD-less", dxgi, getMemProps))
        return false;

    // One timeline both ways: Vulkan signals odd values (copied in), D3D12 even ones (callbacks done)
    HANDLE fh = nullptr;
    if (b.d3d->CreateSharedHandle(b.fence, nullptr, GENERIC_ALL, nullptr, &fh) != S_OK)
        return Fail("CreateSharedHandle (fence)");
    VkSemaphoreTypeCreateInfo tci { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci };
    if (vkCreateSemaphore(b.device, &sci, nullptr, &b.timeline) != VK_SUCCESS)
    {
        CloseHandle(fh);
        return Fail("vkCreateSemaphore (timeline)");
    }
    VkImportSemaphoreWin32HandleInfoKHR isi { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
    isi.semaphore = b.timeline;
    isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    isi.handle = fh;
    const bool semOk = importSem(b.device, &isi) == VK_SUCCESS;
    CloseHandle(fh);
    if (!semOk)
        return Fail("vkImportSemaphoreWin32HandleKHR");
    b.value = b.fence->GetCompletedValue();

    VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = b.family;
    if (vkCreateCommandPool(b.device, &pci, nullptr, &b.pool) != VK_SUCCESS)
        return Fail("vkCreateCommandPool");

    for (auto& s : b.slots)
    {
        VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = b.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 2;
        VkCommandBuffer cbs[2] {};
        if (vkAllocateCommandBuffers(b.device, &cai, cbs) != VK_SUCCESS)
            return Fail("vkAllocateCommandBuffers");
        s.toDx12 = cbs[0];
        s.back = cbs[1];
        VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo bsi { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateFence(b.device, &fci, nullptr, &s.fence) != VK_SUCCESS ||
            vkCreateSemaphore(b.device, &bsi, nullptr, &s.done) != VK_SUCCESS)
            return Fail("vkCreateFence / vkCreateSemaphore");
    }

    b.ready = true;
    LOG_INFO("Vulkan present bridge ready: {}x{} vk format {} -> dxgi {}, queue family {}", b.extent.width,
             b.extent.height, (int) b.format, (int) dxgi, b.family);
    return true;
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                             VkAccessFlags dst)
{
    VkImageMemoryBarrier m { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    m.oldLayout = from;
    m.newLayout = to;
    m.srcAccessMask = src;
    m.dstAccessMask = dst;
    m.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    m.image = image;
    m.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return m;
}

void Copy(VkCommandBuffer cb, VkImage src, VkImage dst, VkExtent2D e)
{
    VkImageCopy r {};
    r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.dstSubresource = r.srcSubresource;
    r.extent = { e.width, e.height, 1 };
    vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

void FillFrame(OptiPresentFrame& f)
{
    auto& b = _b;
    f.size = sizeof(OptiPresentFrame);
    f.api = OptiApi_D3D12;
    f.device = b.d3d;
    f.queue = b.queue;
    f.swapchain = nullptr;
    f.backBuffer = b.frame.res;
    f.backBufferState = D3D12_RESOURCE_STATE_COMMON;
    f.colorSpace = ToDxgiColorSpace(b.colorSpace);
    f.width = b.extent.width;
    f.height = b.extent.height;
    f.format = ToDxgi(b.format);
    f.frameGeneration = OptiFrameGen_Off;
    f.frameId = ++b.frames;
    f.upscaleFrameId = PluginApi::LastUpscaleFrameId();
    if (b.hudlessFrame == f.frameId)
    {
        f.hudless = b.hudless.res;
        f.hudlessState = D3D12_RESOURCE_STATE_COMMON;
        f.hudlessSource = OptiHudless_Hudfix;
    }
}
} // namespace

bool VkPresentDx12::Wanted() { return PluginApi::WantsBridgedPresents(); }

bool VkPresentDx12::HudlessSink(VkExtent2D* extent, VkFormat* format)
{
    if (!Wanted())
        return false;
    std::lock_guard lock(_mutex);
    if (!_b.ready || _b.failed)
        return false;
    if (extent != nullptr)
        *extent = _b.extent;
    if (format != nullptr)
        *format = _b.format;
    return true;
}

bool VkPresentDx12::CaptureHudless(VkCommandBuffer cb, VkImage image, VkImageLayout layout)
{
    std::lock_guard lock(_mutex);
    auto& b = _b;
    if (!b.ready || b.failed || b.hudless.image == VK_NULL_HANDLE)
        return false;

    VkImageMemoryBarrier in[2] = {
        Barrier(image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT),
        Barrier(b.hudless.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, in);
    Copy(cb, image, b.hudless.image, b.extent);
    VkImageMemoryBarrier out[2] = {
        Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
        Barrier(b.hudless.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, out);
    // belongs to the next present
    b.hudlessFrame = b.frames + 1;
    return true;
}

void VkPresentDx12::AdjustSwapchainInfo(VkPhysicalDevice pd, VkSwapchainCreateInfoKHR* info)
{
    if (!Wanted() || pd == VK_NULL_HANDLE)
        return;
    VkSurfaceCapabilitiesKHR caps {};
    constexpr VkImageUsageFlags need = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, info->surface, &caps) == VK_SUCCESS &&
        (caps.supportedUsageFlags & need) == need)
        info->imageUsage |= need;
}

void VkPresentDx12::SwapchainCreated(VkDevice device, VkPhysicalDevice pd, VkSwapchainKHR swapchain,
                                     const VkSwapchainCreateInfoKHR* info)
{
    if (!Wanted())
        return;
    std::lock_guard lock(_mutex);
    if (_b.device == device && _b.ready)
        Release();
    _b.device = device;
    _b.pd = pd;
    _b.swapchain = swapchain;
    _b.format = info->imageFormat;
    _b.colorSpace = info->imageColorSpace;
    _b.extent = info->imageExtent;
    constexpr VkImageUsageFlags need = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    _b.usable = (info->imageUsage & need) == need && info->imageArrayLayers == 1;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
    _b.images.resize(n);
    if (n == 0 || vkGetSwapchainImagesKHR(device, swapchain, &n, _b.images.data()) != VK_SUCCESS)
        _b.usable = false;
    LOG_INFO("Vulkan present bridge: swapchain {:X} {}x{} format {} colour space {} images {} usable {}",
             (uint64_t) swapchain, _b.extent.width, _b.extent.height, (int) _b.format, (int) _b.colorSpace, n,
             _b.usable);
}

void VkPresentDx12::SwapchainDestroyed(VkDevice device, VkSwapchainKHR swapchain)
{
    std::lock_guard lock(_mutex);
    if (_b.swapchain != swapchain || swapchain == VK_NULL_HANDLE)
        return;
    Release();
    _b.swapchain = VK_NULL_HANDLE;
    _b.images.clear();
    _b.usable = false;
}

void VkPresentDx12::QueuePresent(VkQueue queue, VkPresentInfoKHR* info)
{
    if (!Wanted() || info->swapchainCount != 1)
        return;

    std::lock_guard lock(_mutex);
    auto& b = _b;
    if (b.failed || !b.usable || info->pSwapchains[0] != b.swapchain || info->pImageIndices[0] >= b.images.size())
        return;

    uint32_t family = UINT32_MAX;
    if (!MenuOverlayVk::QueueFamily(queue, &family))
        return;
    if (b.ready && family != b.family)
        Release();
    if (!b.ready)
    {
        b.family = family;
        if (!CreateD3D12() || !CreateShared())
            return;
    }

    auto& s = b.slots[b.frames % kSlots];
    vkWaitForFences(b.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(b.device, 1, &s.fence);
    const VkImage image = b.images[info->pImageIndices[0]];

    // 1. after the game's semaphores: swapchain image -> shared image
    VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.toDx12, &bi);
    VkImageMemoryBarrier in[2] = {
        Barrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT),
        Barrier(b.frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    vkCmdPipelineBarrier(s.toDx12, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 2, in);
    Copy(s.toDx12, image, b.frame.image, b.extent);
    auto toGeneral = Barrier(b.frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    vkCmdPipelineBarrier(s.toDx12, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toGeneral);
    vkEndCommandBuffer(s.toDx12);

    const uint64_t copied = ++b.value;
    const uint64_t processed = ++b.value;

    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    std::vector<uint64_t> waitValues(info->waitSemaphoreCount, 0);
    VkTimelineSemaphoreSubmitInfo t1 { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    t1.waitSemaphoreValueCount = info->waitSemaphoreCount;
    t1.pWaitSemaphoreValues = waitValues.data();
    t1.signalSemaphoreValueCount = 1;
    t1.pSignalSemaphoreValues = &copied;
    VkSubmitInfo s1 { VK_STRUCTURE_TYPE_SUBMIT_INFO, &t1 };
    s1.waitSemaphoreCount = info->waitSemaphoreCount;
    s1.pWaitSemaphores = info->pWaitSemaphores;
    s1.pWaitDstStageMask = stages.data();
    s1.commandBufferCount = 1;
    s1.pCommandBuffers = &s.toDx12;
    s1.signalSemaphoreCount = 1;
    s1.pSignalSemaphores = &b.timeline;
    if (vkQueueSubmit(queue, 1, &s1, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        Fail("vkQueueSubmit (copy in)");
        return;
    }

    // 2. the present callbacks on the D3D12 queue, in place on the shared image
    b.queue->Wait(b.fence, copied);
    OptiPresentFrame f {};
    FillFrame(f);
    f.phase = OptiPresent_BeforeOverlay;
    PluginApi::RunPresentCallbacks(&f);
    f.phase = OptiPresent_AfterOverlay;
    PluginApi::RunPresentCallbacks(&f);
    b.queue->Signal(b.fence, processed);

    // 3. shared image -> swapchain image, then the present waits on s.done
    vkBeginCommandBuffer(s.back, &bi);
    VkImageMemoryBarrier out[2] = {
        Barrier(b.frame.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    vkCmdPipelineBarrier(s.back, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, out);
    Copy(s.back, b.frame.image, image, b.extent);
    auto toPresent = Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkCmdPipelineBarrier(s.back, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toPresent);
    vkEndCommandBuffer(s.back);

    const VkPipelineStageFlags backStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const uint64_t zero = 0;
    VkTimelineSemaphoreSubmitInfo t2 { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    t2.waitSemaphoreValueCount = 1;
    t2.pWaitSemaphoreValues = &processed;
    t2.signalSemaphoreValueCount = 1;
    t2.pSignalSemaphoreValues = &zero;
    VkSubmitInfo s2 { VK_STRUCTURE_TYPE_SUBMIT_INFO, &t2 };
    s2.waitSemaphoreCount = 1;
    s2.pWaitSemaphores = &b.timeline;
    s2.pWaitDstStageMask = &backStage;
    s2.commandBufferCount = 1;
    s2.pCommandBuffers = &s.back;
    s2.signalSemaphoreCount = 1;
    s2.pSignalSemaphores = &s.done;
    if (vkQueueSubmit(queue, 1, &s2, s.fence) != VK_SUCCESS)
    {
        // the game's semaphores were consumed by the copy in
        info->waitSemaphoreCount = 0;
        Fail("vkQueueSubmit (copy back)");
        return;
    }

    info->waitSemaphoreCount = 1;
    info->pWaitSemaphores = &s.done;

    if (b.frames == 1 || b.frames % 3000 == 0)
        LOG_INFO("Vulkan present bridge: {} presents through the D3D12 callbacks", b.frames);
}
