#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

int main(void)
{
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Armada VR semaphore capability check",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkInstance instance;
    VkResult result = vkCreateInstance(&create, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", result);
        return 1;
    }

    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0) {
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
        candidates += supported == 2 ? 1 : 0;
    }
    free(devices);
    vkDestroyInstance(instance, NULL);
    if (!candidates) {
        puts("STEAMVR_INTEROP_UNAVAILABLE: no device advertises the required semaphore sharing.");
        return 2;
    }
    puts("STEAMVR_INTEROP_ADVERTISED: export/import behavior and SteamVR rendering still need testing.");
    return 0;
}
