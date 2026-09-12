/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); abort(); } } while (0)
#define HANDLE(type, n) ((type)(uintptr_t)(n))
static unsigned live_allocations;
static int fail_alloc = -1;
static void *allocate(size_t count, size_t size)
{
    if (fail_alloc == 0) return NULL;
    if (fail_alloc > 0) --fail_alloc;
    void *p = calloc(count, size);
    if (p) ++live_allocations;
    return p;
}
static void release(void *p) { if (p) { CHECK(live_allocations); --live_allocations; free(p); } }
static void *resize(void *p, size_t size)
{
    if (fail_alloc == 0) { release(p); return NULL; }
    if (fail_alloc > 0) --fail_alloc;
    if (!p) return allocate(1, size);
    void *next = realloc(p, size);
    if (!next) release(p);
    return next;
}
#define free release
#define U_TYPED_ARRAY_CALLOC(type, n) ((type *)allocate(n, sizeof(type)))
#define U_ARRAY_REALLOC_OR_FREE(ptr, type, n) ((ptr) = (type *)resize(ptr, sizeof(type) * (n)))
#define COMP_ERROR(c, ...) ((void)(c))
#define COMP_DEBUG(c, ...) ((void)(c))
#define COMP_INFO(c, ...) ((void)(c))
#define COMP_PRINT_MODE(c, ...) ((void)(c))
#define CVK_ERROR(c, ...) ((void)(c))
#define vk_print_result(...) ((void)0)
#define vk_result_string(r) "modeled Vulkan result"
#define vk_print_display_surface_create_info(...) ((void)0)
#define VK_NAME_SURFACE(...) ((void)0)

struct vk_bundle {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR vkGetPhysicalDeviceDisplayPropertiesKHR;
    PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR vkGetPhysicalDeviceDisplayPlanePropertiesKHR;
    PFN_vkGetDisplayModePropertiesKHR vkGetDisplayModePropertiesKHR;
    PFN_vkGetDisplayPlaneSupportedDisplaysKHR vkGetDisplayPlaneSupportedDisplaysKHR;
    PFN_vkGetDisplayPlaneCapabilitiesKHR vkGetDisplayPlaneCapabilitiesKHR;
    PFN_vkCreateDisplayPlaneSurfaceKHR vkCreateDisplayPlaneSurfaceKHR;
};
struct comp_compositor {
    struct { struct vk_bundle vk; } base;
    struct { int desired_mode, vk_display, display; } settings;
    int64_t frame_interval_ns;
};
struct comp_target { struct comp_compositor *c; };
struct comp_target_swapchain {
    struct comp_target base;
    struct { VkSurfaceKHR handle; } surface;
    VkExtent2D extent;
};
struct vk_display { VkDisplayPropertiesKHR display_properties; VkDisplayKHR display; };
struct comp_window_vk_display { struct comp_target_swapchain base; struct vk_display *displays; uint16_t display_count; };
static void comp_target_swapchain_override_extents(struct comp_target_swapchain *cts, VkExtent2D extent) { cts->extent = extent; }

static VkDisplayPropertiesKHR displays[3];
static VkDisplayPlanePropertiesKHR planes[4];
static VkDisplayModePropertiesKHR modes[3];
static VkDisplayPlaneCapabilitiesKHR caps[4];
static VkDisplayKHR supported[4][3];
static uint32_t display_count, plane_count, mode_count, supported_count[4];
static VkResult query_failure, fill_failure, capability_failure, surface_failure;
static VkDisplaySurfaceCreateInfoKHR created;
static unsigned surface_calls, capability_calls;

static VkResult enumerate(uint32_t *count, void *out, const void *data, uint32_t length, size_t element)
{
    if (!out) { *count = length; return query_failure; }
    if (fill_failure != VK_SUCCESS) return fill_failure;
    uint32_t n = *count < length ? *count : length;
    memcpy(out, data, element * n); *count = n;
    return n < length ? VK_INCOMPLETE : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL get_displays(VkPhysicalDevice p, uint32_t *count, VkDisplayPropertiesKHR *out)
{ (void)p; return enumerate(count, out, displays, display_count, sizeof(*out)); }
static VKAPI_ATTR VkResult VKAPI_CALL get_planes(VkPhysicalDevice p, uint32_t *count, VkDisplayPlanePropertiesKHR *out)
{ (void)p; return enumerate(count, out, planes, plane_count, sizeof(*out)); }
static VKAPI_ATTR VkResult VKAPI_CALL get_modes(VkPhysicalDevice p, VkDisplayKHR d, uint32_t *count, VkDisplayModePropertiesKHR *out)
{ (void)p; (void)d; return enumerate(count, out, modes, mode_count, sizeof(*out)); }
static VKAPI_ATTR VkResult VKAPI_CALL get_supported(VkPhysicalDevice p, uint32_t plane, uint32_t *count, VkDisplayKHR *out)
{ (void)p; CHECK(plane < plane_count); return enumerate(count, out, supported[plane], supported_count[plane], sizeof(*out)); }
static VKAPI_ATTR VkResult VKAPI_CALL get_caps(VkPhysicalDevice p, VkDisplayModeKHR m, uint32_t plane, VkDisplayPlaneCapabilitiesKHR *out)
{ (void)p; (void)m; CHECK(plane < plane_count); ++capability_calls; if (capability_failure) return capability_failure; *out = caps[plane]; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL create_surface(VkInstance i, const VkDisplaySurfaceCreateInfoKHR *info, const VkAllocationCallbacks *a, VkSurfaceKHR *out)
{ (void)i; (void)a; ++surface_calls; created = *info; if (surface_failure) return surface_failure; *out = HANDLE(VkSurfaceKHR, 99); return VK_SUCCESS; }

#include "enumerate-under-test.h"
#if MONADO_WINDOW_TEST
#include "vk_display-under-test.h"
#else
#include "direct-under-test.h"
#endif

static struct comp_compositor c;
static void reset(void)
{
    CHECK(live_allocations == 0);
    memset(&c, 0, sizeof(c));
    c.base.vk = (struct vk_bundle){.instance = HANDLE(VkInstance, 1), .physical_device = HANDLE(VkPhysicalDevice, 2),
        .vkGetPhysicalDeviceDisplayPropertiesKHR = get_displays, .vkGetPhysicalDeviceDisplayPlanePropertiesKHR = get_planes,
        .vkGetDisplayModePropertiesKHR = get_modes, .vkGetDisplayPlaneSupportedDisplaysKHR = get_supported,
        .vkGetDisplayPlaneCapabilitiesKHR = get_caps, .vkCreateDisplayPlaneSurfaceKHR = create_surface};
    c.settings.desired_mode = 0; c.settings.display = -1; c.frame_interval_ns = 11111111;
    memset(displays, 0, sizeof(displays)); memset(planes, 0, sizeof(planes)); memset(supported, 0, sizeof(supported));
    display_count = 1; plane_count = 2; mode_count = 2; fail_alloc = -1;
    displays[0] = (VkDisplayPropertiesKHR){.display = HANDLE(VkDisplayKHR, 7), .physicalResolution = {640,480},
        .displayName = "test", .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR};
    modes[0] = (VkDisplayModePropertiesKHR){.displayMode = HANDLE(VkDisplayModeKHR, 10), .parameters = {.visibleRegion={640,480}, .refreshRate=72000}};
    modes[1] = (VkDisplayModePropertiesKHR){.displayMode = HANDLE(VkDisplayModeKHR, 11), .parameters = {.visibleRegion={640,480}, .refreshRate=90000}};
    for (unsigned i=0;i<4;i++) {
        supported_count[i] = 1; supported[i][0] = displays[0].display;
        caps[i] = (VkDisplayPlaneCapabilitiesKHR){.supportedAlpha=VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
            .minSrcExtent={1,1}, .maxSrcExtent={UINT32_MAX,UINT32_MAX}, .minDstExtent={1,1}, .maxDstExtent={UINT32_MAX,UINT32_MAX}};
        planes[i].currentStackIndex=i;
    }
    query_failure=fill_failure=capability_failure=surface_failure=VK_SUCCESS;
    surface_calls=capability_calls=0;
}

#if MONADO_WINDOW_TEST
static bool window(void)
{
    struct comp_window_vk_display w = {.base = {.base = {.c = &c}}};
    bool ok = comp_window_vk_display_init(&w.base.base);
    if (ok) {
        CHECK(w.display_count==1 && w.displays[0].display==displays[c.settings.vk_display].display);
        CHECK(w.base.extent.width==displays[c.settings.vk_display].physicalResolution.width);
    }
    free(w.displays); CHECK(live_allocations==0); return ok;
}
int main(void)
{
    alarm(10); reset(); c.settings.vk_display=1;
    CHECK(!window()); // Exactly count reproduced an out-of-bounds access upstream.
#if !MONADO_BASELINE
    reset(); c.settings.vk_display=-1; CHECK(!window());
    reset(); c.settings.vk_display=INT_MAX; CHECK(!window());
    reset(); display_count=0; CHECK(!window());
    reset(); c.base.vk.instance=VK_NULL_HANDLE; CHECK(!window());
    reset(); query_failure=VK_ERROR_DEVICE_LOST; CHECK(!window());
    reset(); fill_failure=VK_INCOMPLETE; CHECK(!window());
    reset(); fail_alloc=0; CHECK(!window());
    reset(); fail_alloc=1; CHECK(!window());
    reset(); c.settings.display=1; CHECK(!window());
    reset(); display_count=2; displays[1]=displays[0]; displays[1].display=HANDLE(VkDisplayKHR,8);
    c.settings.vk_display=1; CHECK(window());
    for (int i=0;i<1024;i++) { reset(); CHECK(window()); }
#endif
    puts("PASS: actual display initialization; empty/boundary/negative indices, Vulkan query failures, allocation cleanup, explicit second display, 1024 lifecycles.");
}
#else
static VkResult surface(void)
{
    struct comp_target_swapchain cts={.base={.c=&c}};
    VkResult r=comp_window_direct_create_surface(&cts,displays[0].display,640,480);
    CHECK(live_allocations==0);
    if (r==VK_SUCCESS) CHECK(cts.surface.handle==HANDLE(VkSurfaceKHR,99));
    else CHECK(cts.surface.handle==VK_NULL_HANDLE);
    return r;
}
int main(void)
{
    alarm(10); reset(); plane_count=0;
    CHECK(surface()!=VK_SUCCESS && surface_calls==0); // Upstream dereferenced empty plane inventory.
#if !MONADO_BASELINE
    int invalid[]={-2,2,INT_MAX};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        reset(); c.settings.desired_mode=invalid[i]; CHECK(surface()!=VK_SUCCESS && surface_calls==0);
    }
    reset(); mode_count=0; CHECK(surface()!=VK_SUCCESS && surface_calls==0);
    reset(); modes[0].parameters.refreshRate=0; CHECK(surface()!=VK_SUCCESS && surface_calls==0);
    reset(); modes[0].parameters.visibleRegion.width=0; CHECK(surface()!=VK_SUCCESS && surface_calls==0);
    reset(); modes[0].parameters.visibleRegion.height=0; CHECK(surface()!=VK_SUCCESS && surface_calls==0);
    reset(); modes[0].displayMode=VK_NULL_HANDLE; CHECK(surface()!=VK_SUCCESS && surface_calls==0);
    reset(); display_count=0; CHECK(surface()!=VK_SUCCESS && surface_calls==0);
    reset(); displays[0].supportedTransforms=VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR; CHECK(surface()!=VK_SUCCESS && surface_calls==0);
    reset(); planes[0].currentDisplay=HANDLE(VkDisplayKHR,8); CHECK(surface()==VK_SUCCESS && created.planeIndex==1);
    reset(); supported[0][0]=HANDLE(VkDisplayKHR,8); CHECK(surface()==VK_SUCCESS && created.planeIndex==1);
    reset(); supported_count[0]=0; CHECK(surface()==VK_SUCCESS && created.planeIndex==1);
    reset(); caps[0].supportedAlpha=0; CHECK(surface()==VK_SUCCESS && created.planeIndex==1);
    reset(); caps[0].maxSrcExtent.width=639; CHECK(surface()==VK_SUCCESS && created.planeIndex==1);
    reset(); caps[0].minDstExtent.height=481; CHECK(surface()==VK_SUCCESS && created.planeIndex==1);
    reset(); caps[0].minSrcPosition.x=1; CHECK(surface()==VK_SUCCESS && created.planeIndex==1);
    reset(); caps[0].maxDstPosition.y=-1; CHECK(surface()==VK_SUCCESS && created.planeIndex==1);
    reset(); caps[0].supportedAlpha=VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR; CHECK(surface()==VK_SUCCESS && created.alphaMode==VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR);
    reset(); caps[0].supportedAlpha=VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR; CHECK(surface()==VK_SUCCESS && created.alphaMode==VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR);
    reset(); caps[0].supportedAlpha=VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR; CHECK(surface()==VK_SUCCESS && created.alphaMode==VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR);
    reset(); capability_failure=VK_ERROR_DEVICE_LOST; CHECK(surface()==VK_ERROR_DEVICE_LOST && surface_calls==0);
    reset(); query_failure=VK_ERROR_DEVICE_LOST; CHECK(surface()!=VK_SUCCESS && surface_calls==0);
    reset(); fill_failure=VK_INCOMPLETE; CHECK(surface()!=VK_SUCCESS && surface_calls==0);
    reset(); surface_failure=VK_ERROR_OUT_OF_HOST_MEMORY; CHECK(surface()==VK_ERROR_OUT_OF_HOST_MEMORY && surface_calls==1);
    for (int i=0;i<4;i++) { reset(); fail_alloc=i; CHECK(surface()!=VK_SUCCESS && surface_calls==0); }
    reset(); c.settings.desired_mode=1; CHECK(surface()==VK_SUCCESS && created.displayMode==modes[1].displayMode && c.frame_interval_ns==11111111);
    reset(); CHECK(surface()==VK_SUCCESS && created.displayMode==modes[0].displayMode && c.frame_interval_ns==13888888);
    reset(); c.settings.desired_mode=-1; CHECK(surface()==VK_SUCCESS && created.displayMode==modes[1].displayMode);
    reset(); modes[0].parameters.visibleRegion=(VkExtent2D){UINT32_MAX,UINT32_MAX};
    CHECK(choose_best_vk_mode_auto(&(struct comp_target){.c=&c},modes,2)==0);
    for (int i=0;i<1024;i++) { reset(); CHECK(surface()==VK_SUCCESS && created.planeIndex==0 && created.alphaMode==VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR); }
#endif
    puts("PASS: actual surface/mode/plane selection; invalid indices and modes, compatible later plane, alpha/transform/extent constraints, allocation and Vulkan failures, explicit refresh, overflow, 1024 cleanup cycles.");
}
#endif
