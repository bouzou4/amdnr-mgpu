// Vulkan probes: device order and groups, external memory / semaphores against D3D12 cross-adapter objects,
// host-pointer import on both cards, WSI from the render card, and the copy cost of each send path.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include "common.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>

namespace {

#define VKF(n) PFN_##n n = nullptr
VKF(vkGetInstanceProcAddr);
VKF(vkCreateInstance);
VKF(vkEnumerateInstanceVersion);
VKF(vkDestroyInstance);
VKF(vkEnumeratePhysicalDevices);
VKF(vkEnumeratePhysicalDeviceGroups);
VKF(vkGetPhysicalDeviceProperties2);
VKF(vkGetPhysicalDeviceFeatures2);
VKF(vkGetPhysicalDeviceMemoryProperties);
VKF(vkGetPhysicalDeviceQueueFamilyProperties);
VKF(vkEnumerateDeviceExtensionProperties);
VKF(vkGetPhysicalDeviceExternalBufferProperties);
VKF(vkGetPhysicalDeviceExternalSemaphoreProperties);
VKF(vkGetPhysicalDeviceImageFormatProperties2);
VKF(vkCreateDevice);
VKF(vkGetDeviceProcAddr);
VKF(vkCreateWin32SurfaceKHR);
VKF(vkDestroySurfaceKHR);
VKF(vkGetPhysicalDeviceSurfaceSupportKHR);
VKF(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
VKF(vkGetPhysicalDeviceSurfacePresentModesKHR);
VKF(vkGetPhysicalDeviceSurfaceFormatsKHR);
#undef VKF

struct Dev
{
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice d = VK_NULL_HANDLE;
    VkQueue q = VK_NULL_HANDLE;
    uint32_t qf = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem {};
    float tsPeriod = 1;
    bool hostPtr = false, win32Mem = false, win32Sem = false, swapchain = false;
    uint64_t hostAlign = 0;
#define DF(n) PFN_##n n = nullptr
    DF(vkDestroyDevice);
    DF(vkGetDeviceQueue);
    DF(vkCreateCommandPool);
    DF(vkAllocateCommandBuffers);
    DF(vkBeginCommandBuffer);
    DF(vkEndCommandBuffer);
    DF(vkResetCommandBuffer);
    DF(vkQueueSubmit);
    DF(vkQueueWaitIdle);
    DF(vkDeviceWaitIdle);
    DF(vkCreateBuffer);
    DF(vkDestroyBuffer);
    DF(vkCreateImage);
    DF(vkDestroyImage);
    DF(vkGetBufferMemoryRequirements);
    DF(vkGetImageMemoryRequirements);
    DF(vkAllocateMemory);
    DF(vkFreeMemory);
    DF(vkBindBufferMemory);
    DF(vkBindImageMemory);
    DF(vkMapMemory);
    DF(vkUnmapMemory);
    DF(vkCmdCopyBuffer);
    DF(vkCmdCopyBufferToImage);
    DF(vkCmdCopyImageToBuffer);
    DF(vkCmdPipelineBarrier);
    DF(vkCmdClearColorImage);
    DF(vkCmdWriteTimestamp);
    DF(vkCreateQueryPool);
    DF(vkGetQueryPoolResults);
    DF(vkCmdResetQueryPool);
    DF(vkCreateSemaphore);
    DF(vkDestroySemaphore);
    DF(vkImportSemaphoreWin32HandleKHR);
    DF(vkGetSemaphoreCounterValue);
    DF(vkGetMemoryHostPointerPropertiesEXT);
    DF(vkGetMemoryWin32HandlePropertiesKHR);
    DF(vkCreateSwapchainKHR);
    DF(vkDestroySwapchainKHR);
    DF(vkGetSwapchainImagesKHR);
    DF(vkAcquireNextImageKHR);
    DF(vkQueuePresentKHR);
    DF(vkCreateFence);
    DF(vkWaitForFences);
    DF(vkResetFences);
    DF(vkDestroyFence);
#undef DF

    void Load()
    {
#define L(n) n = (PFN_##n) vkGetDeviceProcAddr(d, #n)
        L(vkDestroyDevice), L(vkGetDeviceQueue), L(vkCreateCommandPool), L(vkAllocateCommandBuffers);
        L(vkBeginCommandBuffer), L(vkEndCommandBuffer), L(vkResetCommandBuffer), L(vkQueueSubmit), L(vkQueueWaitIdle);
        L(vkDeviceWaitIdle), L(vkCreateBuffer), L(vkDestroyBuffer), L(vkCreateImage), L(vkDestroyImage);
        L(vkGetBufferMemoryRequirements), L(vkGetImageMemoryRequirements), L(vkAllocateMemory), L(vkFreeMemory);
        L(vkBindBufferMemory), L(vkBindImageMemory), L(vkMapMemory), L(vkUnmapMemory), L(vkCmdCopyBuffer);
        L(vkCmdCopyBufferToImage), L(vkCmdCopyImageToBuffer), L(vkCmdPipelineBarrier), L(vkCmdClearColorImage);
        L(vkCmdWriteTimestamp), L(vkCreateQueryPool), L(vkGetQueryPoolResults), L(vkCmdResetQueryPool);
        L(vkCreateSemaphore), L(vkDestroySemaphore), L(vkImportSemaphoreWin32HandleKHR), L(vkGetSemaphoreCounterValue);
        L(vkGetMemoryHostPointerPropertiesEXT), L(vkGetMemoryWin32HandlePropertiesKHR), L(vkCreateSwapchainKHR);
        L(vkDestroySwapchainKHR), L(vkGetSwapchainImagesKHR), L(vkAcquireNextImageKHR), L(vkQueuePresentKHR);
        L(vkCreateFence), L(vkWaitForFences), L(vkResetFences), L(vkDestroyFence);
#undef L
    }

    void Begin()
    {
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
    }
    VkResult SubmitWait(const VkSemaphore* signal = nullptr, const uint64_t* value = nullptr)
    {
        vkEndCommandBuffer(cb);
        VkTimelineSemaphoreSubmitInfo ti { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        if (signal)
        {
            ti.signalSemaphoreValueCount = 1;
            ti.pSignalSemaphoreValues = value;
            si.pNext = &ti;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = signal;
        }
        VkResult r = vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
        if (r != VK_SUCCESS)
            return r;
        return vkQueueWaitIdle(q);
    }

    int MemType(uint32_t bits, VkMemoryPropertyFlags want) const
    {
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want)
                return int(i);
        return -1;
    }
};

struct Buf
{
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    void* map = nullptr;
};

VkInstance g_inst = VK_NULL_HANDLE;

std::string Vr(VkResult r) { return "VkResult " + std::to_string(int(r)); }

bool LoadVulkan()
{
    HMODULE m = LoadLibraryW(L"vulkan-1.dll");
    if (!m)
        return false;
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) GetProcAddress(m, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance) vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
    vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion) vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion");
    return vkCreateInstance != nullptr;
}

void LoadInstance()
{
#define L(n) n = (PFN_##n) vkGetInstanceProcAddr(g_inst, #n)
    L(vkDestroyInstance), L(vkEnumeratePhysicalDevices), L(vkEnumeratePhysicalDeviceGroups);
    L(vkGetPhysicalDeviceProperties2), L(vkGetPhysicalDeviceFeatures2), L(vkGetPhysicalDeviceMemoryProperties);
    L(vkGetPhysicalDeviceQueueFamilyProperties), L(vkEnumerateDeviceExtensionProperties);
    L(vkGetPhysicalDeviceExternalBufferProperties), L(vkGetPhysicalDeviceExternalSemaphoreProperties);
    L(vkGetPhysicalDeviceImageFormatProperties2), L(vkCreateDevice), L(vkGetDeviceProcAddr);
    L(vkCreateWin32SurfaceKHR), L(vkDestroySurfaceKHR), L(vkGetPhysicalDeviceSurfaceSupportKHR);
    L(vkGetPhysicalDeviceSurfaceCapabilitiesKHR), L(vkGetPhysicalDeviceSurfacePresentModesKHR);
    L(vkGetPhysicalDeviceSurfaceFormatsKHR);
#undef L
}

LUID PdLuid(VkPhysicalDevice pd, bool* valid = nullptr)
{
    VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 p { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
    vkGetPhysicalDeviceProperties2(pd, &p);
    LUID l {};
    memcpy(&l, id.deviceLUID, sizeof l);
    if (valid)
        *valid = id.deviceLUIDValid;
    return l;
}

bool SameLuid(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

std::vector<std::string> DevExts(VkPhysicalDevice pd)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> e(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, e.data());
    std::vector<std::string> v;
    for (auto& x : e)
        v.push_back(x.extensionName);
    return v;
}

bool Has(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }

bool MakeDev(Dev& d, VkPhysicalDevice pd)
{
    d.pd = pd;
    auto ex = DevExts(pd);
    std::vector<const char*> en;
    for (const char* e : { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
                           VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME })
        if (Has(ex, e))
            en.push_back(e);
    d.win32Mem = Has(ex, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    d.win32Sem = Has(ex, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
    d.hostPtr = Has(ex, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    d.swapchain = Has(ex, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf.data());
    for (uint32_t i = 0; i < n; ++i)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            d.qf = i;
            break;
        }
    float prio = 1;
    VkDeviceQueueCreateInfo qi { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = d.qf;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    VkDeviceCreateInfo ci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12 };
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = uint32_t(en.size());
    ci.ppEnabledExtensionNames = en.data();
    if (vkCreateDevice(pd, &ci, nullptr, &d.d) != VK_SUCCESS)
        return false;
    d.Load();
    d.vkGetDeviceQueue(d.d, d.qf, 0, &d.q);
    VkCommandPoolCreateInfo pi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = d.qf;
    d.vkCreateCommandPool(d.d, &pi, nullptr, &d.pool);
    VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = d.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    d.vkAllocateCommandBuffers(d.d, &ai, &d.cb);
    vkGetPhysicalDeviceMemoryProperties(pd, &d.mem);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 p2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp };
    vkGetPhysicalDeviceProperties2(pd, &p2);
    d.hostAlign = hp.minImportedHostPointerAlignment;
    d.tsPeriod = p2.properties.limits.timestampPeriod;
    return true;
}

// A plain buffer: host-visible (mapped) or device-local.
Buf MakeBuf(Dev& d, VkDeviceSize size, bool host)
{
    Buf b;
    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    d.vkCreateBuffer(d.d, &bi, nullptr, &b.b);
    VkMemoryRequirements mr;
    d.vkGetBufferMemoryRequirements(d.d, b.b, &mr);
    VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = uint32_t(d.MemType(mr.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    d.vkAllocateMemory(d.d, &ai, nullptr, &b.m);
    d.vkBindBufferMemory(d.d, b.b, b.m, 0);
    if (host)
        d.vkMapMemory(d.d, b.m, 0, VK_WHOLE_SIZE, 0, &b.map);
    return b;
}

void FreeBuf(Dev& d, Buf& b)
{
    if (b.b)
        d.vkDestroyBuffer(d.d, b.b, nullptr);
    if (b.m)
        d.vkFreeMemory(d.d, b.m, nullptr);
    b = {};
}

// A buffer over imported memory: a Win32 handle (D3D12 heap / resource) or a host pointer.
Buf ImportBuf(Dev& d, VkDeviceSize size, VkExternalMemoryHandleTypeFlagBits type, HANDLE h, void* host, VkResult* res,
              std::string* how)
{
    Buf b;
    VkExternalMemoryBufferCreateInfo eb { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    eb.handleTypes = type;
    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb };
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    *res = d.vkCreateBuffer(d.d, &bi, nullptr, &b.b);
    if (*res != VK_SUCCESS)
    {
        *how = "vkCreateBuffer";
        return b;
    }
    VkMemoryRequirements mr;
    d.vkGetBufferMemoryRequirements(d.d, b.b, &mr);
    uint32_t bits = mr.memoryTypeBits;
    if (host)
    {
        VkMemoryHostPointerPropertiesEXT hp { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
        VkResult r = d.vkGetMemoryHostPointerPropertiesEXT(d.d, type, host, &hp);
        if (r != VK_SUCCESS)
        {
            *res = r;
            *how = "vkGetMemoryHostPointerPropertiesEXT";
            return b;
        }
        bits &= hp.memoryTypeBits;
    }
    else if (d.vkGetMemoryWin32HandlePropertiesKHR)
    {
        VkMemoryWin32HandlePropertiesKHR wp { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        if (d.vkGetMemoryWin32HandlePropertiesKHR(d.d, type, h, &wp) == VK_SUCCESS && wp.memoryTypeBits)
            bits &= wp.memoryTypeBits;
    }
    VkImportMemoryHostPointerInfoEXT ih { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
    ih.handleType = type;
    ih.pHostPointer = host;
    VkImportMemoryWin32HandleInfoKHR iw { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    iw.handleType = type;
    iw.handle = h;
    VkMemoryDedicatedAllocateInfo ded { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.buffer = b.b;
    if (type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT)
        iw.pNext = &ded;
    VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, host ? (void*) &ih : (void*) &iw };
    ai.allocationSize = host ? size : std::max<VkDeviceSize>(mr.size, size);
    *res = VK_ERROR_UNKNOWN;
    for (uint32_t i = 0; i < d.mem.memoryTypeCount; ++i)
    {
        if (!(bits & (1u << i)))
            continue;
        ai.memoryTypeIndex = i;
        *res = d.vkAllocateMemory(d.d, &ai, nullptr, &b.m);
        if (*res == VK_SUCCESS)
        {
            char x[96];
            snprintf(x, sizeof x, "memory type %u flags 0x%X", i, d.mem.memoryTypes[i].propertyFlags);
            *how = x;
            break;
        }
    }
    if (*res != VK_SUCCESS)
    {
        char x[96];
        snprintf(x, sizeof x, "vkAllocateMemory over types 0x%X", bits);
        *how = x;
        return b;
    }
    *res = d.vkBindBufferMemory(d.d, b.b, b.m, 0);
    if (*res != VK_SUCCESS)
        *how = "vkBindBufferMemory";
    return b;
}

// Write a pattern into dst through a host-visible staging buffer on the device.
VkResult VkWrite(Dev& d, Buf& dst, const std::vector<uint8_t>& pat)
{
    Buf st = MakeBuf(d, pat.size(), true);
    memcpy(st.map, pat.data(), pat.size());
    d.Begin();
    VkBufferCopy c { 0, 0, pat.size() };
    d.vkCmdCopyBuffer(d.cb, st.b, dst.b, 1, &c);
    VkResult r = d.SubmitWait();
    FreeBuf(d, st);
    return r;
}

std::string ExtMemFeatures(VkExternalMemoryFeatureFlags f, VkExternalMemoryHandleTypeFlags compat)
{
    std::string s;
    if (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
        s += "dedicated-only ";
    if (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)
        s += "exportable ";
    if (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)
        s += "importable ";
    char x[48];
    snprintf(x, sizeof x, "compat 0x%X", compat);
    return s + x;
}

HWND OffscreenWindow()
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"xapi_probe_vk";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"xapi_probe_vk", WS_POPUP, -4000,
                             -4000, 256, 256, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

void Barrier(Dev& d, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    d.vkCmdPipelineBarrier(d.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &b);
}

void Wsi(Dev& R, Dev& D)
{
    HWND wnd = OffscreenWindow();
    VkWin32SurfaceCreateInfoKHR si { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    si.hinstance = GetModuleHandleW(nullptr);
    si.hwnd = wnd;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkResult r = vkCreateWin32SurfaceKHR(g_inst, &si, nullptr, &surf);
    if (r != VK_SUCCESS)
    {
        Result("vk.wsi.surface", false, Vr(r));
        DestroyWindow(wnd);
        return;
    }
    VkBool32 sr = 0, sd = 0;
    vkGetPhysicalDeviceSurfaceSupportKHR(R.pd, R.qf, surf, &sr);
    vkGetPhysicalDeviceSurfaceSupportKHR(D.pd, D.qf, surf, &sd);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(R.pd, surf, &n, nullptr);
    std::vector<VkPresentModeKHR> pm(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(R.pd, surf, &n, pm.data());
    std::string modes;
    for (auto m : pm)
        modes += std::to_string(int(m)) + " ";
    Info("vk.wsi.surface_support", std::string("render card ") + (sr ? "yes" : "no") + ", display card " +
                                       (sd ? "yes" : "no") + "; present modes on R: " + modes +
                                       "(0 immediate, 1 mailbox, 2 fifo, 3 fifo_relaxed)");
    if (sr && R.swapchain)
    {
        VkSurfaceCapabilitiesKHR caps {};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(R.pd, surf, &caps);
        VkSwapchainCreateInfoKHR sc { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        sc.surface = surf;
        sc.minImageCount = std::max(2u, caps.minImageCount);
        sc.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        sc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sc.imageExtent = { 256, 256 };
        sc.imageArrayLayers = 1;
        sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sc.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sc.presentMode = std::find(pm.begin(), pm.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != pm.end()
                             ? VK_PRESENT_MODE_IMMEDIATE_KHR
                             : VK_PRESENT_MODE_FIFO_KHR;
        sc.clipped = VK_TRUE;
        VkSwapchainKHR swap = VK_NULL_HANDLE;
        r = R.vkCreateSwapchainKHR(R.d, &sc, nullptr, &swap);
        std::string det = "create " + Vr(r);
        if (r == VK_SUCCESS)
        {
            uint32_t ic = 0;
            R.vkGetSwapchainImagesKHR(R.d, swap, &ic, nullptr);
            std::vector<VkImage> imgs(ic);
            R.vkGetSwapchainImagesKHR(R.d, swap, &ic, imgs.data());
            VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkFence acq;
            R.vkCreateFence(R.d, &fi, nullptr, &acq);
            int ok = 0;
            VkResult last = VK_SUCCESS;
            for (int i = 0; i < 30; ++i)
            {
                uint32_t idx = 0;
                last = R.vkAcquireNextImageKHR(R.d, swap, 1000000000ull, VK_NULL_HANDLE, acq, &idx);
                if (last < 0)
                    break;
                R.vkWaitForFences(R.d, 1, &acq, VK_TRUE, UINT64_MAX);
                R.vkResetFences(R.d, 1, &acq);
                R.Begin();
                Barrier(R, imgs[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkClearColorValue c { { float(i & 1), 0.2f, 0.4f, 1 } };
                VkImageSubresourceRange rr { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                R.vkCmdClearColorImage(R.cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rr);
                Barrier(R, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                R.SubmitWait();
                VkPresentInfoKHR pi { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                pi.swapchainCount = 1;
                pi.pSwapchains = &swap;
                pi.pImageIndices = &idx;
                last = R.vkQueuePresentKHR(R.q, &pi);
                ok += last >= 0;
            }
            det += ", images " + std::to_string(ic) + ", presents ok " + std::to_string(ok) + "/30, last " + Vr(last);
            R.vkDeviceWaitIdle(R.d);
            R.vkDestroyFence(R.d, acq, nullptr);
            R.vkDestroySwapchainKHR(R.d, swap, nullptr);
        }
        Result("vk.wsi.swapchain_from_render_card", r == VK_SUCCESS, det);
    }
    vkDestroySurfaceKHR(g_inst, surf, nullptr);
    DestroyWindow(wnd);
}

constexpr UINT W = 3840, H = 2160;
constexpr size_t BYTES = size_t(W) * 4 * H;

void Functional(const Adapter& ra, const Adapter& da, Dev& R, Dev& D, D12& R12, D12& D12d)
{
    const VkDeviceSize size = 64ull << 20;
    auto pat = Pattern(size, 11);

    // External buffer capabilities on the render card.
    for (auto [t, n] : { std::pair { VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, "opaque_win32" },
                         std::pair { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT, "d3d12_heap" },
                         std::pair { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, "d3d12_resource" },
                         std::pair { VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, "host_allocation" } })
    {
        VkPhysicalDeviceExternalBufferInfo bi { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.handleType = t;
        VkExternalBufferProperties p { VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
        vkGetPhysicalDeviceExternalBufferProperties(R.pd, &bi, &p);
        Info((std::string("vk.extbuf.") + n).c_str(),
             ExtMemFeatures(p.externalMemoryProperties.externalMemoryFeatures,
                            p.externalMemoryProperties.compatibleHandleTypes));
    }
    {
        VkPhysicalDeviceExternalSemaphoreInfo si { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO };
        VkSemaphoreTypeCreateInfo tc { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        tc.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        si.pNext = &tc;
        si.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        VkExternalSemaphoreProperties p { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
        vkGetPhysicalDeviceExternalSemaphoreProperties(R.pd, &si, &p);
        Info("vk.extsem.d3d12_fence_timeline", "features 0x" + std::to_string(p.externalSemaphoreFeatures) +
                                                   " (1 exportable, 2 importable)");
    }
    char al[64];
    snprintf(al, sizeof al, "R %llu, D %llu bytes", (unsigned long long) R.hostAlign, (unsigned long long) D.hostAlign);
    Info("vk.host_pointer_alignment", al);

    // 1. D3D12 SHARED_CROSS_ADAPTER heap imported into Vulkan on R (heap made on R, and made on D like the ring).
    for (int creator = 0; creator < 2; ++creator)
    {
        D12& C = creator == 0 ? R12 : D12d;
        D12& O = creator == 0 ? D12d : R12;
        D3D12_HEAP_DESC hd {};
        hd.SizeInBytes = size;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        ComPtr<ID3D12Heap> heapC, heapO;
        HANDLE h = nullptr;
        C.dev->CreateHeap(&hd, IID_PPV_ARGS(&heapC));
        C.dev->CreateSharedHandle(heapC.Get(), nullptr, GENERIC_ALL, nullptr, &h);
        O.dev->OpenSharedHandle(h, IID_PPV_ARGS(&heapO));
        VkResult vr;
        std::string how;
        Buf b = ImportBuf(R, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT, h, nullptr, &vr, &how);
        const char* name = creator == 0 ? "vk.import_xadapter_heap_made_on_R" : "vk.import_xadapter_heap_made_on_D";
        if (vr != VK_SUCCESS)
        {
            Result(name, false, how + " " + Vr(vr));
        }
        else
        {
            VkResult w = VkWrite(R, b, pat);
            ID3D12Heap* heapD = creator == 0 ? heapO.Get() : heapC.Get();
            ComPtr<ID3D12Resource> rd;
            auto bd = BufDesc(size, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
            D12d.dev->CreatePlacedResource(heapD, 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&rd));
            std::vector<uint8_t> out;
            bool ok = rd && ReadbackBuffer(D12d, rd.Get(), 0, size, out);
            size_t mm = ok ? Mismatch(out.data(), pat.data(), size) : size;
            Result(name, w == VK_SUCCESS && ok && mm == 0,
                   how + "; Vulkan R writes, D3D12 D reads: mismatched bytes " + std::to_string(mm));
        }
        FreeBuf(R, b);
        CloseHandle(h);
    }

    // 2. Committed cross-adapter D3D12 buffer imported as D3D12_RESOURCE (dedicated).
    {
        HRESULT hr;
        auto res = Committed(R12.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, BufDesc(size, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER),
                             D3D12_RESOURCE_STATE_COMMON, D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, &hr);
        HANDLE h = nullptr;
        if (res)
            R12.dev->CreateSharedHandle(res.Get(), nullptr, GENERIC_ALL, nullptr, &h);
        VkResult vr;
        std::string how;
        Buf b = ImportBuf(R, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, h, nullptr, &vr, &how);
        if (vr != VK_SUCCESS)
        {
            Result("vk.import_xadapter_resource", false, how + " " + Vr(vr));
        }
        else
        {
            VkResult w = VkWrite(R, b, pat);
            ComPtr<ID3D12Resource> rd;
            D12d.dev->OpenSharedHandle(h, IID_PPV_ARGS(&rd));
            std::vector<uint8_t> out;
            bool ok = rd && ReadbackBuffer(D12d, rd.Get(), 0, size, out);
            size_t mm = ok ? Mismatch(out.data(), pat.data(), size) : size;
            Result("vk.import_xadapter_resource", w == VK_SUCCESS && ok && mm == 0,
                   how + "; Vulkan R writes, D3D12 D reads: mismatched bytes " + std::to_string(mm));
        }
        FreeBuf(R, b);
        if (h)
            CloseHandle(h);
    }

    // 3. Host pages imported on both cards (Vulkan R + Vulkan D, and Vulkan R + D3D12 D).
    if (R.hostPtr && D.hostPtr)
    {
        void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        VkResult a, c;
        std::string ha, hc;
        Buf br = ImportBuf(R, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, nullptr, p, &a, &ha);
        Buf bd = ImportBuf(D, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, nullptr, p, &c, &hc);
        Result("vk.host_pointer_import_both", a == VK_SUCCESS && c == VK_SUCCESS,
               "R " + Vr(a) + " " + ha + ", D " + Vr(c) + " " + hc);
        if (a == VK_SUCCESS)
        {
            VkResult w = VkWrite(R, br, pat);
            bool cpu = memcmp(p, pat.data(), size) == 0;
            size_t mmv = size;
            if (c == VK_SUCCESS)
            {
                Buf rb = MakeBuf(D, size, true);
                D.Begin();
                VkBufferCopy cp { 0, 0, size };
                D.vkCmdCopyBuffer(D.cb, bd.b, rb.b, 1, &cp);
                D.SubmitWait();
                mmv = Mismatch((uint8_t*) rb.map, pat.data(), size);
                FreeBuf(D, rb);
            }
            ComPtr<ID3D12Heap> hp;
            ComPtr<ID3D12Resource> rd;
            HRESULT hr = D12d.dev->OpenExistingHeapFromAddress(p, IID_PPV_ARGS(&hp));
            auto bdesc = BufDesc(size, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
            if (hp)
                hr = D12d.dev->CreatePlacedResource(hp.Get(), 0, &bdesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&rd));
            std::vector<uint8_t> out;
            bool ok = rd && ReadbackBuffer(D12d, rd.Get(), 0, size, out);
            size_t mmd = ok ? Mismatch(out.data(), pat.data(), size) : size;
            Result("vk.host_pointer_R_to_D", w == VK_SUCCESS && mmv == 0 && mmd == 0,
                   "Vulkan R writes host pages; CPU sees it " + std::string(cpu ? "yes" : "no") +
                       "; Vulkan D reads: mismatched " + std::to_string(mmv) + "; D3D12 D (OpenExistingHeapFromAddress " +
                       Hr(hr) + ") reads: mismatched " + std::to_string(mmd));
        }
        FreeBuf(R, br);
        FreeBuf(D, bd);
        VirtualFree(p, 0, MEM_RELEASE);
    }

    // 4. Timeline semaphore over a D3D12 SHARED_CROSS_ADAPTER fence: Vulkan R signals, D3D12 D sees it.
    if (R.win32Sem)
    {
        for (int creator = 0; creator < 2; ++creator)
        {
            D12& C = creator == 0 ? R12 : D12d;
            ComPtr<ID3D12Fence> fc, fdv;
            HANDLE h = nullptr;
            C.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&fc));
            C.dev->CreateSharedHandle(fc.Get(), nullptr, GENERIC_ALL, nullptr, &h);
            if (creator == 0)
                D12d.dev->OpenSharedHandle(h, IID_PPV_ARGS(&fdv));
            else
                fdv = fc;
            VkSemaphoreTypeCreateInfo tc { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
            tc.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo sci { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tc };
            VkSemaphore s = VK_NULL_HANDLE;
            R.vkCreateSemaphore(R.d, &sci, nullptr, &s);
            VkImportSemaphoreWin32HandleInfoKHR ii { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
            ii.semaphore = s;
            ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
            ii.handle = h;
            VkResult r = R.vkImportSemaphoreWin32HandleKHR(R.d, &ii);
            const char* name = creator == 0 ? "vk.import_xadapter_fence_made_on_R" : "vk.import_xadapter_fence_made_on_D";
            if (r != VK_SUCCESS)
            {
                Result(name, false, "import " + Vr(r));
            }
            else
            {
                uint64_t v = 7;
                R.Begin();
                VkResult sr = R.SubmitWait(&s, &v);
                Sleep(20);
                uint64_t seen = fdv->GetCompletedValue();
                Result(name, sr == VK_SUCCESS && seen == 7,
                       "Vulkan R submit signal 7: " + Vr(sr) + ", D3D12 fence on D reads " + std::to_string(seen));
            }
            R.vkDestroySemaphore(R.d, s, nullptr);
            CloseHandle(h);
        }
    }

    Wsi(R, D);
}

double TsMs(Dev& d, VkQueryPool qp)
{
    uint64_t t[2] = {};
    d.vkGetQueryPoolResults(d.d, qp, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    return double(t[1] - t[0]) * d.tsPeriod / 1e6;
}

double Median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? -1 : v[v.size() / 2];
}

std::string MsLine(const char* what, double ms, size_t bytes)
{
    char b[160];
    snprintf(b, sizeof b, "%s %.3f ms (%.1f GB/s, %.4f ms/MB)", what, ms, double(bytes) / (ms * 1e6),
             ms / (double(bytes) / 1e6));
    return b;
}

void Bench(Dev& R, D12& R12, D12& D12d)
{
    const int N = 30;
    auto pat = Pattern(BYTES, 21);
    // The game's image on R (optimal tiling) with the pattern.
    VkImageCreateInfo ii { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    ii.extent = { W, H, 1 };
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImage img;
    R.vkCreateImage(R.d, &ii, nullptr, &img);
    VkMemoryRequirements mr;
    R.vkGetImageMemoryRequirements(R.d, img, &mr);
    VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = uint32_t(R.MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    VkDeviceMemory im;
    R.vkAllocateMemory(R.d, &ai, nullptr, &im);
    R.vkBindImageMemory(R.d, img, im, 0);
    {
        Buf st = MakeBuf(R, BYTES, true);
        memcpy(st.map, pat.data(), BYTES);
        R.Begin();
        Barrier(R, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c {};
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageExtent = { W, H, 1 };
        R.vkCmdCopyBufferToImage(R.cb, st.b, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        Barrier(R, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        R.SubmitWait();
        FreeBuf(R, st);
    }
    VkQueryPoolCreateInfo qi { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2;
    VkQueryPool qp;
    R.vkCreateQueryPool(R.d, &qi, nullptr, &qp);

    auto send = [&](Buf& dst) {
        R.Begin();
        R.vkCmdResetQueryPool(R.cb, qp, 0, 2);
        R.vkCmdWriteTimestamp(R.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        VkBufferImageCopy c {};
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageExtent = { W, H, 1 };
        R.vkCmdCopyImageToBuffer(R.cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &c);
        R.vkCmdWriteTimestamp(R.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        R.SubmitWait();
        return TsMs(R, qp);
    };

    // D3D12 ingest on D from a buffer into a texture, plus the end-to-end check.
    auto dTex = Committed(D12d.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, TexDesc(W, H, DXGI_FORMAT_R10G10B10A2_UNORM),
                          D3D12_RESOURCE_STATE_COMMON);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    auto td = TexDesc(W, H, DXGI_FORMAT_R10G10B10A2_UNORM);
    D12d.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
    D3D12_QUERY_HEAP_DESC qd { D12d.copyTs ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 };
    ComPtr<ID3D12QueryHeap> qh;
    D12d.dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&qh));
    auto qrb = Committed(D12d.dev.Get(), D3D12_HEAP_TYPE_READBACK, BufDesc(16), D3D12_RESOURCE_STATE_COPY_DEST);
    auto ingest = [&](ID3D12Resource* src) {
        bool cq = D12d.copyTs;
        D12d.Begin(cq);
        auto l = cq ? D12d.clist.Get() : D12d.list.Get();
        l->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        D3D12_TEXTURE_COPY_LOCATION s { src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        s.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION t { dTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        l->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
        l->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        l->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb.Get(), 0);
        D12d.Submit(cq);
        D12d.Flush(cq);
        uint64_t* p;
        D3D12_RANGE rr { 0, 16 };
        qrb->Map(0, &rr, (void**) &p);
        double ms = double(p[1] - p[0]) * 1000.0 / double(cq ? D12d.tsFreqCopy : D12d.tsFreqDirect);
        qrb->Unmap(0, nullptr);
        return ms;
    };
    auto verify = [&](const char* name) {
        auto tmp = Committed(D12d.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, BufDesc(BYTES), D3D12_RESOURCE_STATE_COMMON);
        D12d.Begin(true);
        D3D12_TEXTURE_COPY_LOCATION s { dTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION t { tmp.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        t.PlacedFootprint = fp;
        D12d.clist->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
        D12d.Submit(true);
        D12d.Flush(true);
        std::vector<uint8_t> out;
        ReadbackBuffer(D12d, tmp.Get(), 0, BYTES, out);
        size_t mm = Mismatch(out.data(), pat.data(), BYTES);
        Result(name, mm == 0, "end-to-end mismatched bytes " + std::to_string(mm));
    };

    const size_t size = (BYTES + 65535) & ~size_t(65535);

    // V0: VRAM baseline (device-local buffer on R).
    {
        Buf b = MakeBuf(R, size, false);
        std::vector<double> s;
        for (int i = 0; i < N; ++i)
            s.push_back(send(b));
        Result("bench.vk_send_vram", true, MsLine("Vulkan image->VRAM buffer", Median(s), BYTES));
        FreeBuf(R, b);
    }
    // V1: D3D12 cross-adapter heap (made on D like the ring) imported into Vulkan R.
    {
        D3D12_HEAP_DESC hd {};
        hd.SizeInBytes = size;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        ComPtr<ID3D12Heap> heapD;
        HANDLE h = nullptr;
        D12d.dev->CreateHeap(&hd, IID_PPV_ARGS(&heapD));
        D12d.dev->CreateSharedHandle(heapD.Get(), nullptr, GENERIC_ALL, nullptr, &h);
        VkResult vr;
        std::string how;
        Buf b = ImportBuf(R, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT, h, nullptr, &vr, &how);
        if (vr == VK_SUCCESS)
        {
            ComPtr<ID3D12Resource> rd;
            auto bd = BufDesc(size, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
            D12d.dev->CreatePlacedResource(heapD.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&rd));
            std::vector<double> s, g;
            for (int i = 0; i < N; ++i)
                s.push_back(send(b)), g.push_back(ingest(rd.Get()));
            Result("bench.vk_send_xadapter_heap", true,
                   MsLine("Vulkan image->imported ring heap", Median(s), BYTES) + "; " + MsLine("D ingest", Median(g), BYTES));
            verify("bench.vk_xadapter_heap_verify");
        }
        else
        {
            Result("bench.vk_send_xadapter_heap", false, how + " " + Vr(vr));
        }
        FreeBuf(R, b);
        CloseHandle(h);
    }
    // V2: host pages imported on Vulkan R and opened by D3D12 D.
    if (R.hostPtr)
    {
        void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        VkResult vr;
        std::string how;
        Buf b = ImportBuf(R, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, nullptr, p, &vr, &how);
        ComPtr<ID3D12Heap> hp;
        ComPtr<ID3D12Resource> rd;
        auto bd = BufDesc(size, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        if (vr == VK_SUCCESS && SUCCEEDED(D12d.dev->OpenExistingHeapFromAddress(p, IID_PPV_ARGS(&hp))) &&
            SUCCEEDED(D12d.dev->CreatePlacedResource(hp.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&rd))))
        {
            std::vector<double> s, g;
            for (int i = 0; i < N; ++i)
                s.push_back(send(b)), g.push_back(ingest(rd.Get()));
            Result("bench.vk_send_host_pages", true,
                   MsLine("Vulkan image->host pages", Median(s), BYTES) + "; " + MsLine("D ingest", Median(g), BYTES));
            verify("bench.vk_host_pages_verify");
        }
        else
        {
            Result("bench.vk_send_host_pages", false, how + " " + Vr(vr));
        }
        rd.Reset(), hp.Reset();
        FreeBuf(R, b);
        VirtualFree(p, 0, MEM_RELEASE);
    }
    R.vkDeviceWaitIdle(R.d);
}

} // namespace

void ProbesVk(const Adapter& ra, const Adapter& da, bool bench)
{
    if (!LoadVulkan())
    {
        Result("vk.loader", false, "vulkan-1.dll");
        return;
    }
    uint32_t iv = 0;
    vkEnumerateInstanceVersion(&iv);
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "xapi_probe";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* ie[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ci { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = ie;
    VkResult r = vkCreateInstance(&ci, nullptr, &g_inst);
    if (r != VK_SUCCESS)
    {
        Result("vk.instance", false, Vr(r));
        return;
    }
    LoadInstance();

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(g_inst, &n, pds.data());
    VkPhysicalDevice pr = VK_NULL_HANDLE, pdD = VK_NULL_HANDLE;
    std::string order;
    for (uint32_t i = 0; i < n; ++i)
    {
        bool valid = false;
        LUID l = PdLuid(pds[i], &valid);
        VkPhysicalDeviceProperties2 p { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        vkGetPhysicalDeviceProperties2(pds[i], &p);
        char x[200];
        snprintf(x, sizeof x, "[%u] bus %d %s luid%s; ", i, valid ? PciBus(l) : -1, p.properties.deviceName,
                 valid ? "" : " (invalid)");
        order += x;
        if (SameLuid(l, ra.luid))
            pr = pds[i];
        if (SameLuid(l, da.luid))
            pdD = pds[i];
    }
    char ver[64];
    snprintf(ver, sizeof ver, "loader %u.%u.%u; ", VK_API_VERSION_MAJOR(iv), VK_API_VERSION_MINOR(iv), VK_API_VERSION_PATCH(iv));
    Info("vk.physical_device_order", ver + order);

    uint32_t gn = 0;
    vkEnumeratePhysicalDeviceGroups(g_inst, &gn, nullptr);
    std::vector<VkPhysicalDeviceGroupProperties> gp(gn, { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES });
    vkEnumeratePhysicalDeviceGroups(g_inst, &gn, gp.data());
    std::string groups = std::to_string(gn) + " groups:";
    for (auto& g : gp)
        groups += " " + std::to_string(g.physicalDeviceCount) + (g.subsetAllocation ? "(subset)" : "");
    Info("vk.device_groups", groups);

    if (!pr || !pdD)
    {
        Result("vk.match_luids", false, "render/display card not found by LUID");
        return;
    }
    auto ex = DevExts(pr);
    std::string want;
    for (const char* e : { "VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32", "VK_EXT_external_memory_host",
                           "VK_KHR_win32_keyed_mutex", "VK_EXT_full_screen_exclusive", "VK_KHR_present_id",
                           "VK_KHR_present_wait", "VK_EXT_swapchain_maintenance1", "VK_AMD_anti_lag", "VK_EXT_hdr_metadata",
                           "VK_KHR_swapchain", "VK_EXT_pci_bus_info", "VK_KHR_present_id2", "VK_KHR_present_wait2",
                           "VK_EXT_present_timing", "VK_KHR_swapchain_maintenance1" })
        want += std::string(e) + (Has(ex, e) ? "=1 " : "=0 ");
    Info("vk.render_card_extensions", want);

    Dev R, D;
    if (!MakeDev(R, pr) || !MakeDev(D, pdD))
    {
        Result("vk.devices", false, "vkCreateDevice");
        return;
    }
    D12 R12, D12d;
    if (!R12.Init(ra.a.Get()) || !D12d.Init(da.a.Get()))
    {
        Result("vk.d3d12_devices", false, "");
        return;
    }
    if (!bench)
        Functional(ra, da, R, D, R12, D12d);
    else
        Bench(R, R12, D12d);
    R.vkDeviceWaitIdle(R.d);
    D.vkDeviceWaitIdle(D.d);
}
