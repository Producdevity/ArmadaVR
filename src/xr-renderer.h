#pragma once

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <array>
#include <vector>

class XrRenderer {
public:
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    unsigned submitted = 0;
    std::array<unsigned, 2> pressed_frames{};
    void initialize(XrInstance instance, XrSystemId system);
    void create_swapchains(XrSession session);
    XrFrameState begin(XrSession session);
    void end(XrSession session, XrSpace space, const XrFrameState &frame,
             const std::array<XrView, 2> &views, const std::array<bool, 2> &pressed);
    void release_swapchains();
    ~XrRenderer();

private:
    static constexpr uint32_t extent = 128;
    std::array<XrSwapchain, 2> swapchains{};
    std::array<std::vector<XrSwapchainImageVulkan2KHR>, 2> images;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
};
