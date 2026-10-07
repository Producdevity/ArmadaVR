#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static PFN_vkGetDeviceProcAddr next_device;
static PFN_vkGetInstanceProcAddr next_instance;
static pthread_once_t initialized = PTHREAD_ONCE_INIT;

static void initialize(void) {
    next_device = (PFN_vkGetDeviceProcAddr)dlsym(RTLD_NEXT, "vkGetDeviceProcAddr");
    next_instance = (PFN_vkGetInstanceProcAddr)dlsym(RTLD_NEXT, "vkGetInstanceProcAddr");
    if (!next_device || !next_instance) {
        void *loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (loader) {
            next_device = (PFN_vkGetDeviceProcAddr)dlsym(loader, "vkGetDeviceProcAddr");
            next_instance = (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
        }
    }
    if (!next_device || !next_instance) {
        fputs("Vulkan resolver requires the real Vulkan loader after this library\n", stderr);
        abort();
    }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
    pthread_once(&initialized, initialize);
    /* FEX's Vulkan guest thunk omits this core command from its generated invoker map. */
    if (device && !strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    return next_device(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    pthread_once(&initialized, initialize);
    if (instance && !strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    return next_instance(instance, name);
}
