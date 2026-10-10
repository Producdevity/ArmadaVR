#define VK_USE_PLATFORM_XCB_KHR
#include <assert.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>
static atomic_uint errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT type,
                                            const VkDebugUtilsMessengerCallbackDataEXT *data,
                                            void *user) {
    (void)type;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        atomic_fetch_add(&errors, 1);
        fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    }
    return VK_FALSE;
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            fprintf(stderr, "%s: %d at line %d\n", #x, r_, __LINE__);                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static int has(const char *name, VkExtensionProperties *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(p[i].extensionName, name))
            return 1;
    return 0;
}
static uint64_t nanos(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static unsigned fd_count(void) {
    DIR *d = opendir("/proc/self/fd");
    assert(d);
    unsigned n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n - 1;
}
static xcb_window_t display_window(xcb_connection_t *connection, xcb_window_t root) {
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(connection, xcb_query_tree(connection, root), NULL);
    assert(tree);
    xcb_window_t found = 0;
    int n = xcb_query_tree_children_length(tree);
    xcb_window_t *windows = xcb_query_tree_children(tree);
    const char name[] = "Armada virtual X11 display";
    for (int i = 0; i < n; i++) {
        xcb_get_property_reply_t *p = xcb_get_property_reply(
            connection,
            xcb_get_property(connection, 0, windows[i], XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 64),
            NULL);
        if (p && xcb_get_property_value_length(p) == (int)strlen(name) &&
            !memcmp(xcb_get_property_value(p), name, strlen(name))) {
            assert(!found);
            found = windows[i];
        }
        free(p);
    }
    free(tree);
    return found;
}
#include <pthread.h>

struct surface_worker {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDisplaySurfaceCreateInfoKHR info;
};
static void *surface_worker_main(void *arg) {
    struct surface_worker *w = arg;
    for (unsigned i = 0; i < 40; i++) {
        VkSurfaceKHR surface;
        CHECK(vkCreateDisplayPlaneSurfaceKHR(w->instance, &w->info, NULL, &surface));
        VkSurfaceCapabilitiesKHR caps;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(w->physical, surface, &caps));
        assert(caps.currentExtent.width == 1024 && caps.minImageCount == 2);
        vkDestroySurfaceKHR(w->instance, surface, NULL);
    }
    return NULL;
}
struct allocation_test {
    unsigned attempt, fail_at, live, injected;
    void *pointers[128];
    size_t sizes[128];
};
static void *test_allocate(void *user, size_t size, size_t alignment,
                           VkSystemAllocationScope scope) {
    (void)scope;
    struct allocation_test *t = user;
    if (++t->attempt == t->fail_at) {
        t->injected++;
        return NULL;
    }
    void *p = NULL;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    if (posix_memalign(&p, alignment, size))
        return NULL;
    for (unsigned i = 0; i < 128; i++)
        if (!t->pointers[i]) {
            t->pointers[i] = p;
            t->sizes[i] = size;
            t->live++;
            return p;
        }
    abort();
}
static void test_free(void *user, void *p) {
    if (!p)
        return;
    struct allocation_test *t = user;
    for (unsigned i = 0; i < 128; i++)
        if (t->pointers[i] == p) {
            t->pointers[i] = NULL;
            t->live--;
            free(p);
            return;
        }
    abort();
}
static void *test_reallocate(void *user, void *old, size_t size, size_t alignment,
                             VkSystemAllocationScope scope) {
    if (!old)
        return test_allocate(user, size, alignment, scope);
    if (!size) {
        test_free(user, old);
        return NULL;
    }
    struct allocation_test *t = user;
    size_t old_size = 0;
    for (unsigned i = 0; i < 128; i++)
        if (t->pointers[i] == old)
            old_size = t->sizes[i];
    assert(old_size);
    void *p = test_allocate(user, size, alignment, scope);
    if (p) {
        memcpy(p, old, size < old_size ? size : old_size);
        test_free(user, old);
    }
    return p;
}
static void surface_lifetime_tests(VkInstance instance, VkPhysicalDevice physical,
                                   const VkDisplaySurfaceCreateInfoKHR *info,
                                   xcb_connection_t *connection, xcb_window_t root) {
    unsigned failed = 0, passed = 0;
    for (unsigned at = 1; at <= 12; at++) {
        struct allocation_test t = {.fail_at = at};
        VkAllocationCallbacks allocator = {.pUserData = &t,
                                           .pfnAllocation = test_allocate,
                                           .pfnReallocation = test_reallocate,
                                           .pfnFree = test_free};
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult result = vkCreateDisplayPlaneSurfaceKHR(instance, info, &allocator, &surface);
        if (result == VK_SUCCESS) {
            VkBool32 supported = VK_FALSE;
            result = vkGetPhysicalDeviceSurfaceSupportKHR(physical, 0, surface, &supported);
            assert(result == VK_SUCCESS || result == VK_ERROR_OUT_OF_HOST_MEMORY);
            if (result == VK_SUCCESS && supported) {
                VkSurfaceCapabilitiesKHR caps;
                result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
                assert(result == VK_SUCCESS || result == VK_ERROR_OUT_OF_HOST_MEMORY);
            }
            vkDestroySurfaceKHR(instance, surface, &allocator);
        }
        printf("SURFACE_ALLOCATION at=%u attempts=%u injected=%u result=%d live=%u\n", at,
               t.attempt, t.injected, result, t.live);
        if (t.injected)
            failed++;
        else {
            assert(result == VK_SUCCESS);
            passed++;
        }
        assert(t.live == 0);
        for (int retry = 0; retry < 80 && display_window(connection, root); retry++)
            usleep(10000);
        assert(!display_window(connection, root));
    }
    assert(failed && passed);
    printf("SURFACE_ALLOCATION_TESTS failed=%u passed=%u live=0\n", failed, passed);
    struct surface_worker work = {.instance = instance, .physical = physical, .info = *info};
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; i++)
        assert(!pthread_create(&threads[i], NULL, surface_worker_main, &work));
    for (unsigned i = 0; i < 8; i++)
        assert(!pthread_join(threads[i], NULL));
    for (int retry = 0; retry < 80 && display_window(connection, root); retry++)
        usleep(10000);
    assert(!display_window(connection, root));
    puts("SURFACE_CONCURRENT_TESTS threads=8 surfaces=320 owned_windows=0");
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "--xcb") && strcmp(argv[1], "--display"))) {
        fprintf(stderr, "Usage: vulkan-display --xcb|--display\n");
        return 2;
    }
    unsigned initial_fds = fd_count();
    int display_path = !strcmp(argv[1], "--display");
    setbuf(stdout, NULL);
    alarm(55);
    uint32_t n = 0;
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &n, NULL));
    VkExtensionProperties *ext = calloc(n, sizeof(*ext));
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &n, ext));
    if (!has(VK_KHR_DISPLAY_EXTENSION_NAME, ext, n)) {
        puts("UNSUPPORTED: KHR_display absent");
        free(ext);
        return 77;
    }
    free(ext);
    const char *ie[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME,
                        VK_KHR_DISPLAY_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "vulkan-display",
                             .apiVersion = VK_API_VERSION_1_2};
    VkDebugUtilsMessengerCreateInfoEXT dbg = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pNext = &dbg,
                                .pApplicationInfo = &app,
                                .enabledExtensionCount = 6,
                                .ppEnabledExtensionNames = ie,
                                .enabledLayerCount = 1,
                                .ppEnabledLayerNames = &layer};
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    PFN_vkCreateDebugUtilsMessengerEXT create_debug =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,
                                                                  "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");
    assert(create_debug && destroy_debug);
    VkDebugUtilsMessengerEXT messenger;
    CHECK(create_debug(instance, &dbg, NULL, &messenger));
    n = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, NULL));
    assert(n == 1);
    VkPhysicalDevice physical;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, &physical));
    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR displays =
        (PFN_vkGetPhysicalDeviceDisplayPropertiesKHR)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    assert(displays);
    n = 0;
    CHECK(displays(physical, &n, NULL));
    printf("display_count=%u\n", n);
    n = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &n, NULL));
    ext = calloc(n, sizeof(*ext));
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &n, ext));
    assert(has(VK_KHR_PRESENT_ID_EXTENSION_NAME, ext, n) &&
           has(VK_KHR_PRESENT_WAIT_EXTENSION_NAME, ext, n));
    free(ext);
    VkPhysicalDevicePresentWaitFeaturesKHR waitf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR idf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .pNext = &waitf};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .pNext = &idf};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &timeline};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    assert(waitf.presentWait && idf.presentId && timeline.timelineSemaphore);
    int screen_num = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_num);
    assert(!xcb_connection_has_error(connection));
    const xcb_setup_t *setup = xcb_get_setup(connection);
    assert(setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++)
        xcb_screen_next(&it);
    xcb_screen_t *screen = it.data;
    assert(screen && screen->root_depth == 24);
    xcb_visualtype_t *visual = NULL;
    for (xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen); di.rem;
         xcb_depth_next(&di))
        for (xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data); vi.rem;
             xcb_visualtype_next(&vi))
            if (vi.data->visual_id == screen->root_visual)
                visual = vi.data;
    assert(visual && visual->red_mask == 0xff0000 && visual->green_mask == 0xff00 &&
           visual->blue_mask == 0xff);
    xcb_window_t window;
    VkSurfaceKHR surface;
    VkDisplayKHR clock_display = VK_NULL_HANDLE;
    VkDisplaySurfaceCreateInfoKHR display_info = {0};
    xcb_generic_error_t *xe = NULL;
    if (display_path) {
        n = 0;
        CHECK(displays(physical, &n, NULL));
        assert(n == 1);
        VkDisplayPropertiesKHR display;
        CHECK(displays(physical, &n, &display));
        clock_display = display.display;
        assert(n == 1 && display.physicalResolution.width == 1024 &&
               display.physicalResolution.height == 512 &&
               !strcmp(display.displayName, "Armada virtual X11 display"));
        n = 0;
        CHECK(vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical, &n, NULL));
        assert(n == 1);
        VkDisplayPlanePropertiesKHR plane;
        CHECK(vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical, &n, &plane));
        assert(plane.currentDisplay == display.display && plane.currentStackIndex == 0);
        n = 0;
        CHECK(vkGetDisplayPlaneSupportedDisplaysKHR(physical, 0, &n, NULL));
        assert(n == 1);
        VkDisplayKHR supported;
        CHECK(vkGetDisplayPlaneSupportedDisplaysKHR(physical, 0, &n, &supported));
        assert(supported == display.display);
        n = 0;
        CHECK(vkGetDisplayModePropertiesKHR(physical, display.display, &n, NULL));
        assert(n == 1);
        VkDisplayModePropertiesKHR mode;
        CHECK(vkGetDisplayModePropertiesKHR(physical, display.display, &n, &mode));
        assert(mode.parameters.visibleRegion.width == 1024 &&
               mode.parameters.visibleRegion.height == 512 && mode.parameters.refreshRate == 60000);
        VkDisplayModeCreateInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR,
                                         .parameters = mode.parameters};
        VkDisplayModeKHR created;
        CHECK(vkCreateDisplayModeKHR(physical, display.display, &mi, NULL, &created));
        assert(created != VK_NULL_HANDLE);
        mode.displayMode = created;
        mi.parameters.refreshRate = 60001;
        VkResult unsupported =
            vkCreateDisplayModeKHR(physical, display.display, &mi, NULL, &created);
        assert(unsupported == VK_ERROR_INITIALIZATION_FAILED);
        VkDisplayPlaneCapabilitiesKHR plane_caps;
        CHECK(vkGetDisplayPlaneCapabilitiesKHR(physical, mode.displayMode, 0, &plane_caps));
        assert(plane_caps.supportedAlpha == VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR &&
               plane_caps.minSrcExtent.width == 1024);
        display_info = (VkDisplaySurfaceCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
            .displayMode = mode.displayMode,
            .planeIndex = 0,
            .planeStackIndex = 0,
            .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .globalAlpha = 1,
            .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
            .imageExtent = {1024, 512}};
        CHECK(vkCreateDisplayPlaneSurfaceKHR(instance, &display_info, NULL, &surface));
        VkSurfaceCapabilitiesKHR first_caps;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &first_caps));
        VkSurfaceKHR duplicate;
        CHECK(vkCreateDisplayPlaneSurfaceKHR(instance, &display_info, NULL, &duplicate));
        VkSurfaceCapabilitiesKHR duplicate_caps;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, duplicate, &duplicate_caps));
        assert(duplicate_caps.currentExtent.width == 1024 &&
               duplicate_caps.currentExtent.height == 512);
        vkDestroySurfaceKHR(instance, duplicate, NULL);
        window = display_window(connection, screen->root);
        assert(window);
        printf("virtual_display_enumeration_mode_surface_pass window=%u unsupported_mode=%d "
               "second_surface_shared_window=1\n",
               window, unsupported);
    } else {
        window = xcb_generate_id(connection);
        uint32_t attrs[] = {1, XCB_EVENT_MASK_EXPOSURE};
        xe = xcb_request_check(
            connection,
            xcb_create_window_checked(connection, screen->root_depth, window, screen->root, 20, 20,
                                      96, 64, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                                      XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, attrs));
        assert(!xe);
        xe = xcb_request_check(connection, xcb_map_window_checked(connection, window));
        assert(!xe);
        xcb_flush(connection);
        VkXcbSurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                                         .connection = connection,
                                         .window = window};
        CHECK(vkCreateXcbSurfaceKHR(instance, &sci, NULL, &surface));
    }
    n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, NULL);
    VkQueueFamilyProperties *q = calloc(n, sizeof(*q));
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, q);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        VkBool32 supported = 0;
        CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supported));
        if (supported && (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            family = i;
            break;
        }
    }
    free(q);
    assert(family != UINT32_MAX);
    const char *de[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PRESENT_ID_EXTENSION_NAME,
                        VK_KHR_PRESENT_WAIT_EXTENSION_NAME, VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME};
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = family,
                                   .queueCount = 1,
                                   .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .pNext = &timeline,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = 4,
                              .ppEnabledExtensionNames = de};
    VkDevice device;
    CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    PFN_vkWaitForPresentKHR wait_present =
        (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(device, "vkWaitForPresentKHR");
    assert(wait_present);
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    assert(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    n = 0;
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, NULL));
    VkSurfaceFormatKHR *formats = calloc(n, sizeof(*formats));
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, formats));
    VkSurfaceFormatKHR format = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
            formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            format = formats[i];
            break;
        }
    }
    free(formats);
    assert(format.format);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
        extent = (VkExtent2D){96, 64};
    assert(extent.width == (display_path ? 1024u : 96u) &&
           extent.height == (display_path ? 512u : 64u));
    if (display_path) {
        assert(caps.minImageCount == 2);
        uint32_t mode_count = 0;
        CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count, NULL));
        assert(mode_count == 1);
        VkPresentModeKHR mode;
        CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count, &mode));
        assert(mode == VK_PRESENT_MODE_FIFO_KHR);
        VkPhysicalDeviceSurfaceInfo2KHR si = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .surface = surface};
        VkSurfaceCapabilities2KHR c2 = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
        CHECK(vkGetPhysicalDeviceSurfaceCapabilities2KHR(physical, &si, &c2));
        assert(c2.surfaceCapabilities.minImageCount == 2);
    }
    uint32_t count = display_path ? 2 : caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha =
        (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha &
                                      (0u - caps.supportedCompositeAlpha));
    VkSwapchainCreateInfoKHR swapinfo = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                         .surface = surface,
                                         .minImageCount = count,
                                         .imageFormat = format.format,
                                         .imageColorSpace = format.colorSpace,
                                         .imageExtent = extent,
                                         .imageArrayLayers = 1,
                                         .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                         .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                         .preTransform = caps.currentTransform,
                                         .compositeAlpha = alpha,
                                         .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                         .clipped = VK_TRUE};
    VkSwapchainCounterCreateInfoEXT counters = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT,
                                               .surfaceCounters = VK_SURFACE_COUNTER_VBLANK_BIT_EXT};
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2EXT get_caps =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilities2EXT)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceSurfaceCapabilities2EXT");
    VkSurfaceCapabilities2EXT counter_caps = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_EXT};
    assert(get_caps);
    CHECK(get_caps(physical, surface, &counter_caps));
    assert(counter_caps.supportedSurfaceCounters ==
           (display_path ? VK_SURFACE_COUNTER_VBLANK_BIT_EXT : 0));
    if (display_path)
        swapinfo.pNext = &counters;
    VkSwapchainKHR swapchain;
    CHECK(vkCreateSwapchainKHR(device, &swapinfo, NULL, &swapchain));
    n = 0;
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &n, NULL));
    if (display_path)
        assert(n == 2);
    VkImage *images = calloc(n, sizeof(*images));
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &n, images));
    VkSemaphore *done = calloc(n, sizeof(*done));
    VkSemaphoreCreateInfo seminfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; i++)
        CHECK(vkCreateSemaphore(device, &seminfo, NULL, &done[i]));
    VkFenceCreateInfo finfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence acquired, submitted;
    CHECK(vkCreateFence(device, &finfo, NULL, &acquired));
    CHECK(vkCreateFence(device, &finfo, NULL, &submitted));
    VkCommandPoolCreateInfo poolinfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                        .queueFamilyIndex = family};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &poolinfo, NULL, &pool));
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &allocate, &cmd));
    VkResult zero = wait_present(device, swapchain, 1, 0);
    uint64_t start = nanos();
    VkResult finite = wait_present(device, swapchain, 1, 1000000);
    uint64_t elapsed = nanos() - start;
    printf("unsubmitted_id zero=%d finite=%d elapsed_ns=%llu\n", zero, finite,
           (unsigned long long)elapsed);
    assert(zero == VK_TIMEOUT && finite == VK_TIMEOUT);
    unsigned pending_timeouts = 0;
    for (uint64_t id = 1; id <= 12; id++) {
        uint32_t index;
        CHECK(
            vkAcquireNextImageKHR(device, swapchain, 2000000000, VK_NULL_HANDLE, acquired, &index));
        CHECK(vkWaitForFences(device, 1, &acquired, VK_TRUE, 2000000000));
        CHECK(vkResetFences(device, 1, &acquired));
        CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[index],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);
        VkClearColorValue color = {{id % 2 ? 1.0f : 0.0f, 0.0f, id % 2 ? 0.0f : 1.0f, 1.0f}};
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                             &barrier.subresourceRange);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                             &barrier);
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               .commandBufferCount = 1,
                               .pCommandBuffers = &cmd,
                               .signalSemaphoreCount = 1,
                               .pSignalSemaphores = &done[index]};
        CHECK(vkQueueSubmit(queue, 1, &submit, submitted));
        VkPresentIdKHR presentid = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &id};
        VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                    .pNext = &presentid,
                                    .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &done[index],
                                    .swapchainCount = 1,
                                    .pSwapchains = &swapchain,
                                    .pImageIndices = &index};
        CHECK(vkQueuePresentKHR(queue, &present));
        CHECK(vkQueueWaitIdle(queue));
        VkResult pending = wait_present(device, swapchain, id, 1000);
        printf("submitted_pending_id=%llu result=%d\n", (unsigned long long)id, pending);
        assert(pending == VK_SUCCESS || pending == VK_TIMEOUT);
        if (pending == VK_TIMEOUT)
            pending_timeouts++;
        CHECK(wait_present(device, swapchain, id, 2000000000));
        CHECK(wait_present(device, swapchain, id, 0));
        CHECK(vkWaitForFences(device, 1, &submitted, VK_TRUE, 2000000000));
        CHECK(vkResetFences(device, 1, &submitted));
        xcb_get_image_reply_t *pixels = xcb_get_image_reply(
            connection,
            xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, extent.width / 2,
                          extent.height / 2, 1, 1, UINT32_MAX),
            &xe);
        assert(pixels && !xe && xcb_get_image_data_length(pixels) >= 4);
        uint32_t pixel;
        memcpy(&pixel, xcb_get_image_data(pixels), 4);
        free(pixels);
        uint32_t expected = id % 2 ? 0xff0000 : 0xff;
        printf("present_id=%llu pixel=%06x expected=%06x\n", (unsigned long long)id,
               pixel & 0xffffff, expected);
        assert((pixel & 0xffffff) == expected);
    }
    if (display_path) {
        PFN_vkRegisterDisplayEventEXT register_event = (void *)vkGetDeviceProcAddr(device, "vkRegisterDisplayEventEXT");
        PFN_vkRegisterDeviceEventEXT register_device = (void *)vkGetDeviceProcAddr(device, "vkRegisterDeviceEventEXT");
        PFN_vkGetSwapchainCounterEXT counter = (void *)vkGetDeviceProcAddr(device, "vkGetSwapchainCounterEXT");
        PFN_vkDisplayPowerControlEXT power = (void *)vkGetDeviceProcAddr(device, "vkDisplayPowerControlEXT");
        assert(register_event && register_device && counter && power);
        unsigned before = fd_count();
        VkDisplayEventInfoEXT event = { .sType = VK_STRUCTURE_TYPE_DISPLAY_EVENT_INFO_EXT,
                                       .displayEvent = VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT };
        uint64_t first, previous, current;
        CHECK(counter(device, swapchain, VK_SURFACE_COUNTER_VBLANK_BIT_EXT, &first));
        previous = first;
        uint64_t begin = nanos();
        for (unsigned sample = 0; sample < 32; sample++) {
            VkFence fence;
            CHECK(register_event(device, clock_display, &event, NULL, &fence));
            VkResult status = vkGetFenceStatus(device, fence);
            assert(status == VK_SUCCESS || status == VK_NOT_READY);
            CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000));
            CHECK(vkGetFenceStatus(device, fence));
            CHECK(counter(device, swapchain, VK_SURFACE_COUNTER_VBLANK_BIT_EXT, &current));
            assert(current > previous);
            printf("CLOCK_EVENT sample=%u msc=%llu delta=%llu\n", sample,
                   (unsigned long long)current, (unsigned long long)(current-previous));
            previous = current;
            vkDestroyFence(device, fence, NULL);
        }
        uint64_t duration = nanos() - begin;
        double rate = (current - first) * 1e9 / duration;
        printf("CLOCK_RATE hz=%f elapsed_ns=%llu ticks=%llu\n", rate,
               (unsigned long long)duration, (unsigned long long)(current-first));
        assert(duration > 250000000 && rate > 50 && rate < 70);
        VkFence pair[2];
        for (unsigned i = 0; i < 2; i++) CHECK(register_event(device, clock_display, &event, NULL, &pair[i]));
        CHECK(vkWaitForFences(device, 2, pair, VK_FALSE, 2000000000));
        CHECK(vkWaitForFences(device, 2, pair, VK_TRUE, 2000000000));
        for (unsigned i = 0; i < 2; i++) vkDestroyFence(device, pair[i], NULL);
        for (unsigned i = 0; i < 32; i++) {
            VkFence fence;
            CHECK(register_event(device, clock_display, &event, NULL, &fence));
            vkDestroyFence(device, fence, NULL);
        }
        VkDeviceEventInfoEXT hotplug = {.sType = VK_STRUCTURE_TYPE_DEVICE_EVENT_INFO_EXT,
                                       .deviceEvent = VK_DEVICE_EVENT_TYPE_DISPLAY_HOTPLUG_EXT};
        VkFence unchanged;
        CHECK(register_device(device, &hotplug, NULL, &unchanged));
        assert(vkWaitForFences(device, 1, &unchanged, VK_TRUE, 1000000) == VK_TIMEOUT);
        vkDestroyFence(device, unchanged, NULL);
        for (unsigned i = 0; i < 3; i++) {
            VkDisplayPowerInfoEXT pi = {.sType = VK_STRUCTURE_TYPE_DISPLAY_POWER_INFO_EXT,
                .powerState = i == 2 ? VK_DISPLAY_POWER_STATE_ON_EXT :
                              i == 1 ? VK_DISPLAY_POWER_STATE_SUSPEND_EXT : VK_DISPLAY_POWER_STATE_OFF_EXT};
            CHECK(power(device, clock_display, &pi));
            xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(
                connection, xcb_get_window_attributes(connection, window), NULL);
            assert(attr && attr->map_state == (i == 2 ? XCB_MAP_STATE_VIEWABLE : XCB_MAP_STATE_UNMAPPED));
            free(attr);
        }
        unsigned after = fd_count();
        printf("CLOCK_FENCES_PASS before_fds=%u after_fds=%u\n", before, after);
        assert(before == after);
    }

    printf("submitted_pending_timeouts=%u\n", pending_timeouts);
    assert(pending_timeouts > 0);
    CHECK(vkDeviceWaitIdle(device));
    for (uint32_t i = 0; i < n; i++)
        vkDestroySemaphore(device, done[i], NULL);
    free(done);
    free(images);
    vkDestroyFence(device, acquired, NULL);
    vkDestroyFence(device, submitted, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    if (display_path) {
        for (int retry = 0; retry < 80 && display_window(connection, screen->root); retry++)
            usleep(10000);
        assert(!display_window(connection, screen->root));
        PFN_vkDisplayPowerControlEXT set_power =
            (PFN_vkDisplayPowerControlEXT)vkGetDeviceProcAddr(device, "vkDisplayPowerControlEXT");
        VkDisplayPowerInfoEXT off = {.sType = VK_STRUCTURE_TYPE_DISPLAY_POWER_INFO_EXT,
                                     .powerState = VK_DISPLAY_POWER_STATE_OFF_EXT};
        CHECK(set_power(device, clock_display, &off));
        CHECK(vkCreateDisplayPlaneSurfaceKHR(instance, &display_info, NULL, &surface));
        VkSurfaceCapabilitiesKHR recreated;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &recreated));
        xcb_window_t powered_window = display_window(connection, screen->root);
        assert(powered_window);
        xcb_get_window_attributes_reply_t *powered_attr = xcb_get_window_attributes_reply(
            connection, xcb_get_window_attributes(connection, powered_window), NULL);
        assert(powered_attr && powered_attr->map_state == XCB_MAP_STATE_UNMAPPED);
        free(powered_attr);
        off.powerState = VK_DISPLAY_POWER_STATE_ON_EXT;
        CHECK(set_power(device, clock_display, &off));
        powered_attr = xcb_get_window_attributes_reply(
            connection, xcb_get_window_attributes(connection, powered_window), NULL);
        assert(powered_attr && powered_attr->map_state == XCB_MAP_STATE_VIEWABLE);
        free(powered_attr);
        puts("DISPLAY_POWER_BEFORE_SURFACE_PASS");

        assert(display_window(connection, screen->root));
        vkDestroySurfaceKHR(instance, surface, NULL);
        for (int retry = 0; retry < 80 && display_window(connection, screen->root); retry++)
            usleep(10000);
        assert(!display_window(connection, screen->root));
        puts("VIRTUAL_DISPLAY_RECREATE_DESTROY_PASS");
        surface_lifetime_tests(instance, physical, &display_info, connection, screen->root);
    } else {
        xcb_destroy_window(connection, window);
        xcb_flush(connection);
    }
    vkDestroyDevice(device, NULL);
    xcb_disconnect(connection);
    destroy_debug(instance, messenger, NULL);
    vkDestroyInstance(instance, NULL);
    unsigned final_fds = fd_count();
    printf("fd_count=%u->%u\n", initial_fds, final_fds);
    assert(initial_fds == final_fds);
    printf("validation_errors=%u\n", errors);
    assert(!errors);
    puts("PRESENT_WAIT_PIXELS_PASS");
    return 0;
}
