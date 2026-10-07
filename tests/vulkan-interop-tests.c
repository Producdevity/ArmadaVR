#include <assert.h>
#include <stdint.h>

#define main interop_main
#include "../src/vulkan-interop.c"
#undef main

static int display_available, command_available, display_error, wait_available;
static int wait_feature, split_devices, oversized_extensions, incomplete_extensions;
static unsigned display_calls, feature_calls, resolver_calls, instances_destroyed;

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char *layer, uint32_t *count, VkExtensionProperties *properties)
{
    assert(!layer);
    if (oversized_extensions) {
        *count = 4097;
        return VK_SUCCESS;
    }
    if (!properties) {
        *count = display_available ? 2 : 0;
        return VK_SUCCESS;
    }
    if (incomplete_extensions) return VK_INCOMPLETE;
    if (display_available) {
        assert(*count >= 2);
        strcpy(properties[0].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
        strcpy(properties[1].extensionName, VK_KHR_DISPLAY_EXTENSION_NAME);
    }
    *count = display_available ? 2 : 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    assert(!allocator);
    assert(info->enabledExtensionCount == 0 || info->enabledExtensionCount == 2);
    if (info->enabledExtensionCount) {
        assert(display_available);
        assert(!strcmp(info->ppEnabledExtensionNames[0], VK_KHR_SURFACE_EXTENSION_NAME));
        assert(!strcmp(info->ppEnabledExtensionNames[1], VK_KHR_DISPLAY_EXTENSION_NAME));
    }
    *instance = (VkInstance)(uintptr_t)1;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
    assert(instance && !allocator);
    ++instances_destroyed;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance,
    uint32_t *count, VkPhysicalDevice *devices)
{
    assert(instance);
    if (devices) {
        assert(*count >= (split_devices ? 2u : 1u));
        devices[0] = (VkPhysicalDevice)(uintptr_t)1;
        if (split_devices) devices[1] = (VkPhysicalDevice)(uintptr_t)2;
    }
    *count = split_devices ? 2 : 1;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice device,
    VkPhysicalDeviceProperties *properties)
{
    assert(device);
    memset(properties, 0, sizeof(*properties));
    strcpy(properties->deviceName, "modeled Vulkan device");
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice device,
    const VkPhysicalDeviceExternalSemaphoreInfo *info, VkExternalSemaphoreProperties *properties)
{
    assert(info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
    if (device == (VkPhysicalDevice)(uintptr_t)1) {
        properties->externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
            VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
        properties->compatibleHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice device,
    const char *layer, uint32_t *count, VkExtensionProperties *properties)
{
    assert(device && !layer);
    if (properties) {
        assert(*count >= (wait_available ? 3u : 2u));
        strcpy(properties[0].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        strcpy(properties[1].extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME);
        if (wait_available) strcpy(properties[2].extensionName, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    }
    *count = wait_available ? 3 : 2;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice device,
    VkPhysicalDeviceFeatures2 *features)
{
    assert(device && wait_available);
    ++feature_calls;
    VkPhysicalDevicePresentIdFeaturesKHR *id = features->pNext;
    assert(id->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
    id->presentId = VK_TRUE;
    VkPhysicalDevicePresentWaitFeaturesKHR *wait = id->pNext;
    assert(wait->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
    wait->presentWait = wait_feature;
}

static VKAPI_ATTR VkResult VKAPI_CALL enumerate_displays(VkPhysicalDevice device,
    uint32_t *count, VkDisplayPropertiesKHR *properties)
{
    assert(display_available && command_available && !properties);
    ++display_calls;
    *count = !split_devices || device == (VkPhysicalDevice)(uintptr_t)2;
    return display_error ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    assert(instance && display_available);
    assert(!strcmp(name, "vkGetPhysicalDeviceDisplayPropertiesKHR"));
    ++resolver_calls;
    return command_available ? (PFN_vkVoidFunction)enumerate_displays : NULL;
}

static void reset(void)
{
    display_available = command_available = wait_available = wait_feature = 1;
    display_error = split_devices = oversized_extensions = incomplete_extensions = 0;
    display_calls = feature_calls = resolver_calls = instances_destroyed = 0;
}

static int run(int display)
{
    char *arguments[] = {"vulkan-interop", "--direct-display", NULL};
    return interop_main(display ? 2 : 1, arguments);
}

int main(void)
{
    reset();
    assert(run(1) == 0 && display_calls == 1 && instances_destroyed == 1);
    reset();
    display_available = 0;
    assert(run(1) == 2 && resolver_calls == 0 && display_calls == 0 && instances_destroyed == 1);
    reset();
    command_available = 0;
    assert(run(1) == 2 && resolver_calls == 1 && display_calls == 0);
    reset();
    display_error = 1;
    assert(run(1) == 2 && display_calls == 1);
    reset();
    wait_feature = 0;
    assert(run(1) == 2 && feature_calls == 1);
    reset();
    wait_available = 0;
    assert(run(1) == 2 && feature_calls == 0);
    reset();
    split_devices = 1;
    assert(run(1) == 2 && display_calls == 2 && instances_destroyed == 1);
    reset();
    display_available = wait_available = 0;
    assert(run(0) == 0 && resolver_calls == 0 && feature_calls == 0);
    reset();
    oversized_extensions = 1;
    assert(run(1) == 1 && instances_destroyed == 0);
    reset();
    incomplete_extensions = 1;
    assert(run(1) == 1 && instances_destroyed == 0);
    puts("VULKAN_INTEROP_QUERY_TESTS_PASS: modeled queries only; no rendering or hardware proof.");
    return 0;
}
