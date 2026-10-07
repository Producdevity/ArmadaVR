#define _GNU_SOURCE
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static PFN_xrGetInstanceProcAddr next_gipa;
static PFN_vkGetInstanceProcAddr vulkan_gipa;
static void *vulkan_gdpa;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void initialize(void) {
    next_gipa = dlsym(RTLD_NEXT, "xrGetInstanceProcAddr");
    // Wine loads these dependencies privately; RTLD_NEXT alone cannot find them.
    if (!next_gipa) {
        void *loader = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
        if (loader) next_gipa = dlsym(loader, "xrGetInstanceProcAddr");
    }
    void *vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (vulkan) {
        vulkan_gipa = dlsym(vulkan, "vkGetInstanceProcAddr");
        vulkan_gdpa = dlsym(vulkan, "vkGetDeviceProcAddr");
    }
}

static XrResult XRAPI_CALL create_device(XrInstance instance, const XrVulkanDeviceCreateInfoKHR *info,
                                        VkDevice *device, VkResult *result) {
    PFN_xrCreateVulkanDeviceKHR original;
    XrResult status = next_gipa(instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&original);
    if (XR_FAILED(status)) return status;
    if (!info) return original(instance, info, device, result);
    XrVulkanDeviceCreateInfoKHR copy = *info;
    if (vulkan_gdpa && (void *)copy.pfnGetInstanceProcAddr == vulkan_gdpa) {
        if (!vulkan_gipa) return XR_ERROR_RUNTIME_FAILURE;
        copy.pfnGetInstanceProcAddr = vulkan_gipa;
        fputs("ARMADA_VR_WINE_OPENXR_INSTANCE_RESOLVER_CORRECTED\n", stderr);
    }
    return original(instance, &copy, device, result);
}

XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
    pthread_once(&once, initialize);
    if (!next_gipa) return XR_ERROR_RUNTIME_FAILURE;
    XrResult result = next_gipa(instance, name, function);
    if (XR_SUCCEEDED(result) && name && !strcmp(name, "xrCreateVulkanDeviceKHR") && function && *function)
        *function = (PFN_xrVoidFunction)create_device;
    return result;
}
