#define XR_USE_GRAPHICS_API_VULKAN
#include "xr-renderer.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

static void xr_check(XrResult result) {
    if (XR_FAILED(result)) throw std::runtime_error("Rendered OpenXR call failed: " + std::to_string(result));
}

static void vk_check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Rendered Vulkan call failed: " + std::to_string(result));
}

template<typename T> static T function(XrInstance instance, const char *name) {
    PFN_xrVoidFunction value = nullptr;
    xr_check(xrGetInstanceProcAddr(instance, name, &value));
    if (!value) throw std::runtime_error(std::string("Missing OpenXR function: ") + name);
    return reinterpret_cast<T>(value);
}

void XrRenderer::initialize(XrInstance instance, XrSystemId system) {
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    xr_check(function<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance, "xrGetVulkanGraphicsRequirements2KHR")(
        instance, system, &requirements));
    if (requirements.minApiVersionSupported > XR_MAKE_VERSION(1, 1, 0) ||
        requirements.maxApiVersionSupported < XR_MAKE_VERSION(1, 1, 0))
        throw std::runtime_error("Vulkan 1.1 is outside the runtime's supported range");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Armada VR input rendering";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    create.systemId = system;
    create.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    create.vulkanCreateInfo = &instance_info;
    VkResult result;
    xr_check(function<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR")(
        instance, &create, &binding.instance, &result));
    vk_check(result);
    XrVulkanGraphicsDeviceGetInfoKHR get{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    get.systemId = system;
    get.vulkanInstance = binding.instance;
    xr_check(function<PFN_xrGetVulkanGraphicsDevice2KHR>(instance, "xrGetVulkanGraphicsDevice2KHR")(
        instance, &get, &binding.physicalDevice));
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(binding.physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(binding.physicalDevice, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == count) throw std::runtime_error("No Vulkan graphics queue");
    binding.queueFamilyIndex = family;
    const float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    XrVulkanDeviceCreateInfoKHR device_create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    device_create.systemId = system;
    device_create.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    device_create.vulkanPhysicalDevice = binding.physicalDevice;
    device_create.vulkanCreateInfo = &device_info;
    xr_check(function<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR")(
        instance, &device_create, &binding.device, &result));
    vk_check(result);
    vkGetDeviceQueue(binding.device, family, 0, &queue);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vk_check(vkCreateCommandPool(binding.device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(binding.device, &allocate, &command));
}

void XrRenderer::create_swapchains(XrSession session) {
    uint32_t count;
    xr_check(xrEnumerateSwapchainFormats(session, 0, &count, nullptr));
    std::vector<int64_t> formats(count);
    xr_check(xrEnumerateSwapchainFormats(session, count, &count, formats.data()));
    auto format = formats.end();
    for (auto candidate : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
                           VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB}) {
        format = std::find(formats.begin(), formats.end(), candidate);
        if (format != formats.end()) break;
    }
    if (format == formats.end()) throw std::runtime_error("Runtime lacks supported 8-bit color swapchains");
    std::printf("OpenXR Vulkan swapchain format=%lld\n", static_cast<long long>(*format));
    for (unsigned eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create.format = *format;
        create.sampleCount = create.faceCount = create.arraySize = create.mipCount = 1;
        create.width = create.height = extent;
        xr_check(xrCreateSwapchain(session, &create, &swapchains[eye]));
        xr_check(xrEnumerateSwapchainImages(swapchains[eye], 0, &count, nullptr));
        if (!count) throw std::runtime_error("Empty OpenXR swapchain");
        images[eye].resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        xr_check(xrEnumerateSwapchainImages(swapchains[eye], count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader *>(images[eye].data())));
    }
}

XrFrameState XrRenderer::begin(XrSession session) {
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    xr_check(xrWaitFrame(session, &wait, &frame));
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    xr_check(xrBeginFrame(session, &begin));
    return frame;
}

void XrRenderer::end(XrSession session, XrSpace space, const XrFrameState &frame,
                     const std::array<XrView, 2> &views, const std::array<bool, 2> &pressed) {
    std::array<XrCompositionLayerProjectionView, 2> projections{};
    if (frame.shouldRender) {
        for (unsigned eye = 0; eye < 2; ++eye) {
            uint32_t index;
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            xr_check(xrAcquireSwapchainImage(swapchains[eye], &acquire, &index));
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            xr_check(xrWaitSwapchainImage(swapchains[eye], &wait));
            vk_check(vkResetCommandBuffer(command, 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            vk_check(vkBeginCommandBuffer(command, &begin));
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.image = images[eye].at(index).image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkClearColorValue color{{eye ? 0.05f : 0.8f, pressed[eye] ? 0.8f : 0.05f, eye ? 0.8f : 0.05f, 1}};
            vkCmdClearColorImage(command, barrier.image, barrier.newLayout, &color, 1, &barrier.subresourceRange);
            barrier.oldLayout = barrier.newLayout;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            vk_check(vkEndCommandBuffer(command));
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            vk_check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
            vk_check(vkQueueWaitIdle(queue));
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xr_check(xrReleaseSwapchainImage(swapchains[eye], &release));
            projections[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projections[eye].pose = views[eye].pose;
            projections[eye].fov = views[eye].fov;
            projections[eye].subImage.swapchain = swapchains[eye];
            projections[eye].subImage.imageRect.extent = {extent, extent};
        }
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = space;
    layer.viewCount = projections.size();
    layer.views = projections.data();
    const auto *header = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = frame.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = frame.shouldRender ? 1 : 0;
    end.layers = &header;
    xr_check(xrEndFrame(session, &end));
    if (frame.shouldRender) {
        ++submitted;
        for (unsigned eye = 0; eye < 2; ++eye) pressed_frames[eye] += pressed[eye];
    }
}

void XrRenderer::release_swapchains() {
    if (binding.device) vkDeviceWaitIdle(binding.device);
    for (auto &swapchain : swapchains) {
        if (swapchain) xrDestroySwapchain(swapchain);
        swapchain = XR_NULL_HANDLE;
    }
}

XrRenderer::~XrRenderer() {
    if (pool) vkDestroyCommandPool(binding.device, pool, nullptr);
    if (binding.device) vkDestroyDevice(binding.device, nullptr);
    if (binding.instance) vkDestroyInstance(binding.instance, nullptr);
}
