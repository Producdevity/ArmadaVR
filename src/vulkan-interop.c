#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static int has_extension(const VkExtensionProperties *extensions, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return 1;
    return 0;
}

static int direct_display(VkInstance instance, VkPhysicalDevice device, int display_extension)
{
    uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL);
    if (result != VK_SUCCESS || count > 4096) return 0;
    VkExtensionProperties *extensions = calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) return 0;
    result = vkEnumerateDeviceExtensionProperties(device, NULL, &count, extensions);
    int wait = result == VK_SUCCESS && has_extension(extensions, count, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    int id = result == VK_SUCCESS && has_extension(extensions, count, VK_KHR_PRESENT_ID_EXTENSION_NAME);
    int swapchain = result == VK_SUCCESS && has_extension(extensions, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    free(extensions);
    VkPhysicalDevicePresentWaitFeaturesKHR wait_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
    };
    VkPhysicalDevicePresentIdFeaturesKHR id_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
        .pNext = &wait_features,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &id_features,
    };
    if (wait && id) vkGetPhysicalDeviceFeatures2(device, &features);
    uint32_t displays = 0;
    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR enumerate = NULL;
    if (display_extension)
        enumerate = (PFN_vkGetPhysicalDeviceDisplayPropertiesKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    if (enumerate) {
        result = enumerate(device, &displays, NULL);
        if (result != VK_SUCCESS) displays = 0;
    }
    printf("  VK_KHR_display: %s; enumerated displays: %u\n", display_extension && enumerate ? "available" : "unavailable", displays);
    printf("  VK_KHR_swapchain: %s\n", swapchain ? "available" : "unavailable");
    printf("  VK_KHR_present_id: %s; presentId feature: %s\n", id ? "available" : "unavailable", id_features.presentId ? "supported" : "unavailable");
    printf("  VK_KHR_present_wait: %s; presentWait feature: %s\n", wait ? "available" : "unavailable", wait_features.presentWait ? "supported" : "unavailable");
    return displays && swapchain && id && wait && id_features.presentId && wait_features.presentWait;
}

int main(int argc, char **argv)
{
    int check_display = argc == 2 && !strcmp(argv[1], "--direct-display");
    if (argc != 1 && !check_display) {
        fputs("Usage: vulkan-interop [--direct-display]\n", stderr);
        return 1;
    }
    uint32_t extension_count = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL);
    if (result != VK_SUCCESS || extension_count > 4096) return 1;
    VkExtensionProperties *extensions = calloc(extension_count ? extension_count : 1, sizeof(*extensions));
    if (!extensions) return 1;
    result = vkEnumerateInstanceExtensionProperties(NULL, &extension_count, extensions);
    int display_extension = check_display && result == VK_SUCCESS &&
        has_extension(extensions, extension_count, VK_KHR_DISPLAY_EXTENSION_NAME) &&
        has_extension(extensions, extension_count, VK_KHR_SURFACE_EXTENSION_NAME);
    free(extensions);
    if (result != VK_SUCCESS) return 1;
    const char *display_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Armada VR semaphore capability check",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = display_extension ? 2 : 0,
        .ppEnabledExtensionNames = display_extension ? display_extensions : NULL,
    };
    VkInstance instance;
    result = vkCreateInstance(&create, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", result);
        return 1;
    }

    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0 || count > 256) {
        fprintf(stderr, "No Vulkan device available (VkResult=%d)\n", result);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) {
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    result = vkEnumeratePhysicalDevices(instance, &count, devices);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkEnumeratePhysicalDevices failed: %d\n", result);
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 1;
    }

    unsigned candidates = 0;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties device;
        vkGetPhysicalDeviceProperties(devices[i], &device);
        printf("Device %u: %s\n", i, device.deviceName);
        unsigned supported = 0;
        for (unsigned timeline = 0; timeline < 2; ++timeline) {
            VkSemaphoreTypeCreateInfo type = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                .semaphoreType = timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY,
            };
            VkPhysicalDeviceExternalSemaphoreInfo query = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
                .pNext = &type,
                .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
            };
            VkExternalSemaphoreProperties properties = {
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
            };
            vkGetPhysicalDeviceExternalSemaphoreProperties(devices[i], &query, &properties);
            VkExternalSemaphoreFeatureFlags required =
                VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
                VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
            int available = (properties.externalSemaphoreFeatures & required) == required &&
                (properties.compatibleHandleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
            printf("  %s OPAQUE_FD import/export: %s (features=0x%x, compatible=0x%x)\n",
                   timeline ? "timeline" : "binary", available ? "available" : "unavailable",
                   properties.externalSemaphoreFeatures, properties.compatibleHandleTypes);
            supported += available ? 1 : 0;
        }
        int display = check_display ? direct_display(instance, devices[i], display_extension) : 1;
        candidates += supported == 2 && display ? 1 : 0;
    }
    free(devices);
    vkDestroyInstance(instance, NULL);
    if (!candidates) {
        puts(check_display ? "STEAMVR_DIRECT_DISPLAY_UNAVAILABLE: no device has the required display, present-wait and semaphore capabilities." :
             "STEAMVR_INTEROP_UNAVAILABLE: no device advertises the required semaphore sharing.");
        return 2;
    }
    puts(check_display ? "STEAMVR_DIRECT_DISPLAY_ADVERTISED: display acquisition, scanout and SteamVR rendering still need testing." :
         "STEAMVR_INTEROP_ADVERTISED: export/import behavior and SteamVR rendering still need testing.");
    return 0;
}
