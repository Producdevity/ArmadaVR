/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include "tu_kgsl_display.h"

static void finish(int failed)
{
   puts(failed ? "KGSL_DISPLAY_FAIL" : "KGSL_DISPLAY_ALL_PASS");
   fflush(NULL);
   sync();
   reboot(RB_POWER_OFF);
   _exit(failed);
}
#define CHECK(expr) do { if (!(expr)) { \
   fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #expr, errno); finish(1); \
} } while (0)
static void expired(int signo) { (void)signo; finish(1); }
static unsigned fd_count(void)
{
   DIR *dir = opendir("/proc/self/fd"); CHECK(dir);
   unsigned n = 0; while (readdir(dir)) ++n;
   CHECK(closedir(dir) == 0); return n;
}


struct wsi_display { int fd; };
static unsigned property_lists, property_records, property_blobs;
static drmModeObjectProperties *counted_properties(int fd, uint32_t id, uint32_t type)
{
   drmModeObjectProperties *p = drmModeObjectGetProperties(fd, id, type);
   property_lists += p != NULL;
   return p;
}
static drmModePropertyPtr counted_property(int fd, uint32_t id)
{
   drmModePropertyPtr p = drmModeGetProperty(fd, id);
   property_records += p != NULL;
   return p;
}
static drmModePropertyBlobPtr counted_blob(int fd, uint32_t id)
{
   drmModePropertyBlobPtr p = drmModeGetPropertyBlob(fd, id);
   property_blobs += p != NULL;
   return p;
}
static void release_properties(drmModeObjectProperties *p)
{
   CHECK(property_lists > 0); --property_lists; drmModeFreeObjectProperties(p);
}
static void release_property(drmModePropertyPtr p)
{
   CHECK(property_records > 0); --property_records; drmModeFreeProperty(p);
}
static void release_blob(drmModePropertyBlobPtr p)
{
   CHECK(property_blobs > 0); --property_blobs; drmModeFreePropertyBlob(p);
}
#define drmModeObjectGetProperties counted_properties
#define drmModeGetProperty counted_property
#define drmModeGetPropertyBlob counted_blob
#define drmModeFreeObjectProperties release_properties
#define drmModeFreeProperty release_property
#define drmModeFreePropertyBlob release_blob
#define mesa_loge(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#include "display-format-under-test.h"
#undef drmModeObjectGetProperties
#undef drmModeGetProperty
#undef drmModeGetPropertyBlob
#undef drmModeFreeObjectProperties
#undef drmModeFreeProperty
#undef drmModeFreePropertyBlob
#undef mesa_loge

#include "mesa-display-image-types.h"
#include <drm_fourcc.h>

static unsigned image_planes, image_fault, image_imports, image_closes, image_close_errors;
static unsigned image_creates, image_destroys;
static int image_dma_fd = -1;
static VkResult wsi_create_image(struct wsi_swapchain *chain, int *info, struct wsi_image *image)
{
   CHECK(info == &chain->image_info);
   struct wsi_display_swapchain *display = (struct wsi_display_swapchain *)chain;
   struct drm_mode_create_dumb dumb = {.width=64, .height=64, .bpp=32};
   CHECK(drmIoctl(display->wsi->fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) == 0);
   CHECK(drmPrimeHandleToFD(display->wsi->fd, dumb.handle, DRM_CLOEXEC | DRM_RDWR, &image_dma_fd) == 0);
   struct drm_gem_close release = {.handle=dumb.handle};
   CHECK(drmIoctl(display->wsi->fd, DRM_IOCTL_GEM_CLOSE, &release) == 0);
   *image = (struct wsi_image){.num_planes=image_planes,
      .dma_buf_fd=image_fault == 1 ? -1 : image_dma_fd,
      .drm_modifier=image_fault == 2 ? UINT64_C(0x0bad000000000001) : DRM_FORMAT_MOD_INVALID};
   for (unsigned i=0; i<image_planes; ++i) {
      image->row_pitches[i]=dumb.pitch;
      image->offsets[i]=i*4096;
   }
   ++image_creates;
   return VK_SUCCESS;
}
static void wsi_destroy_image(struct wsi_swapchain *chain, struct wsi_image *image)
{
   (void)chain; (void)image;
   CHECK(image_dma_fd >= 0 && close(image_dma_fd) == 0);
   image_dma_fd=-1; ++image_destroys;
}
static int image_import(int fd, int dma_fd, uint32_t *handle)
{
   ++image_imports;
   return drmPrimeFDToHandle(fd, dma_fd, handle);
}
static int image_ioctl(int fd, unsigned long request, void *argument)
{
   CHECK(request == DRM_IOCTL_GEM_CLOSE);
   ++image_closes;
   int result = drmIoctl(fd, request, argument);
   if (result) ++image_close_errors;
   return result;
}
#define drmPrimeFDToHandle image_import
#define drmIoctl image_ioctl
#include "display-image-under-test.h"
#undef drmPrimeFDToHandle
#undef drmIoctl

static void check_images(int fd)
{
   struct wsi_display display={fd};
   struct wsi_display_swapchain chain={.wsi=&display};
   VkSwapchainCreateInfoKHR info={.imageExtent={64,64}};
   unsigned cases=0, framebuffers=0, import_refusals=0, fb_refusals=0;
   unsigned initial=fd_count();
   for (unsigned repeat=0; repeat<16; ++repeat) {
      for (image_planes=1; image_planes<=4; ++image_planes) {
         for (image_fault=0; image_fault<3; ++image_fault) {
            image_imports=image_closes=image_close_errors=image_creates=image_destroys=0;
            struct wsi_display_image image={0};
            VkResult result=wsi_display_image_init(&chain.base,&info,DRM_FORMAT_XRGB8888,&image);
            if (!image_fault && image_planes==1) CHECK(result==VK_SUCCESS);
            if (image_fault) CHECK(result==VK_ERROR_OUT_OF_HOST_MEMORY);
            if (result==VK_SUCCESS) {
               ++framebuffers;
               wsi_display_image_finish(&chain.base,&image);
            } else if (image_fault==1) ++import_refusals;
            else ++fb_refusals;
            CHECK(image_imports==1 && image_creates==1 && image_destroys==1);
            CHECK(image_closes==(image_fault==1 ? 0u : 1u) && image_close_errors==0);
            CHECK(image_dma_fd==-1 && fd_count()==initial);
            ++cases;
         }
      }
   }
   CHECK(framebuffers && import_refusals && fb_refusals);
   printf("KGSL_DISPLAY_IMAGE_CASES=%u framebuffers=%u import_refusals=%u framebuffer_refusals=%u\n",
          cases,framebuffers,import_refusals,fb_refusals);
   puts("KGSL_DISPLAY_IMAGE_OWNERSHIP_PASS");
}

static void check_formats(int fd, const drmModePlaneRes *planes)
{
   unsigned cases = 0, with_blob = 0, without_blob = 0;
   struct wsi_display wsi = {fd};
   for (unsigned p = 0; p < planes->count_planes; ++p) {
      uint32_t id = planes->planes[p];
      drmModePlane *plane = drmModeGetPlane(fd, id); CHECK(plane);
      drmModeObjectProperties *props = drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE); CHECK(props);
      drmModePropertyBlobPtr blob = NULL;
      for (unsigned i = 0; i < props->count_props; ++i) {
         drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]); CHECK(prop);
         if (!strcmp(prop->name, "IN_FORMATS") && props->prop_values[i]) {
            CHECK(!blob);
            blob = drmModeGetPropertyBlob(fd, props->prop_values[i]); CHECK(blob);
         }
         drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);
      if (blob) ++with_blob; else ++without_blob;
      for (unsigned repeat = 0; repeat < 32; ++repeat) {
         for (unsigned f = 0; f <= plane->count_formats; ++f) {
            uint32_t format = f < plane->count_formats ? plane->formats[f] : 0;
            uint64_t expected[512]; unsigned expected_count = 0;
            drmModeFormatModifierIterator iter = {0};
            while (blob && drmModeFormatModifierBlobIterNext(blob, &iter)) {
               if (iter.fmt != format) continue;
               CHECK(expected_count < sizeof(expected) / sizeof(expected[0]));
               expected[expected_count++] = iter.mod;
            }
            uint32_t count = UINT32_MAX;
            uint64_t *actual = wsi_get_modifiers_for_format(&wsi, id, format, &count);
            CHECK(count == expected_count && !property_lists && !property_records && !property_blobs);
            CHECK((actual != NULL) == (expected_count != 0));
            if (count) CHECK(memcmp(actual, expected, count * sizeof(*actual)) == 0);
            free(actual);
            ++cases;
         }
      }
      if (blob) drmModeFreePropertyBlob(blob);
      drmModeFreePlane(plane);
   }
   CHECK(cases > 0);
   printf("KGSL_DISPLAY_FORMAT_CASES=%u planes_with_blob=%u planes_without_blob=%u\n", cases, with_blob, without_blob);
   puts("KGSL_DISPLAY_FORMAT_MODIFIERS_PASS");
}

int main(void)
{
   if (getpid() != 1) {
      fputs("Only run as PID 1 in the diskless QEMU test.\n", stderr);
      return 2;
   }
   CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
   char cmdline[4096];
   FILE *cmd = fopen("/proc/cmdline", "r"); CHECK(cmd);
   CHECK(fgets(cmdline, sizeof(cmdline), cmd) && strstr(cmdline, "reframed.kgsl_display_test=1"));
   CHECK(fclose(cmd) == 0);
   signal(SIGALRM, expired); alarm(20);
   CHECK(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0);
   CHECK(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0);
   const char *primary = "/dev/dri/card0", *render = "/dev/dri/renderD128";
   for (unsigned i = 0; i < 100 && access(primary, F_OK); ++i) usleep(10000);
   unsigned initial = fd_count();
   int64_t device_major = -1, device_minor = -1;
   CHECK(tu_kgsl_display_open(NULL, &device_major, &device_minor) == -1 && errno == EINVAL);
   CHECK(tu_kgsl_display_open("/dev/null", &device_major, &device_minor) == -1 && errno == ENODEV);
   int master = tu_kgsl_display_open(primary, &device_major, &device_minor);
   CHECK(master >= 0 && drmIsMaster(master));
   drmVersionPtr version = drmGetVersion(master);
   CHECK(version && strcmp(version->name, "virtio_gpu") == 0);
   drmFreeVersion(version);
   CHECK(fcntl(master, F_GETFD) & FD_CLOEXEC);
   CHECK(tu_kgsl_display_matches_fd(master, device_major, device_minor));
   int render_fd = open(render, O_RDWR | O_CLOEXEC); CHECK(render_fd >= 0);
   CHECK(drmGetNodeTypeFromFd(render_fd) == DRM_NODE_RENDER);
   CHECK(!tu_kgsl_display_matches_fd(render_fd, device_major, device_minor));
   int64_t untouched_major = -5, untouched_minor = -7;
   CHECK(tu_kgsl_display_open(render, &untouched_major, &untouched_minor) == -1 && errno == ENODEV);
   CHECK(untouched_major == -5 && untouched_minor == -7);
   puts("KGSL_DISPLAY_PRIMARY_AND_RENDER_PASS");

   unsigned held = fd_count();
   for (unsigned i = 0; i < 128; ++i) {
      CHECK(tu_kgsl_display_open(primary, &untouched_major, &untouched_minor) == -1 && errno == EACCES);
      CHECK(untouched_major == -5 && untouched_minor == -7);
      CHECK(fd_count() == held && drmIsMaster(master));
   }
   puts("KGSL_DISPLAY_MASTER_CONTENTION_PASS");
   drmModeRes *res = drmModeGetResources(master); CHECK(res);
   drmModePlaneRes *planes = drmModeGetPlaneResources(master); CHECK(planes);
   CHECK(res->count_connectors > 0 && res->count_crtcs > 0 && planes->count_planes > 0);
   check_formats(master, planes);
   check_images(master);
   uint32_t objects[] = {res->connectors[0], res->crtcs[0], planes->planes[0]}, lessee;
   int lease = drmModeCreateLease(master, objects, 3, O_CLOEXEC, &lessee); CHECK(lease >= 0);
   CHECK(tu_kgsl_display_matches_fd(lease, device_major, device_minor));
   drmModeRes *leased = drmModeGetResources(lease); CHECK(leased);
   CHECK(leased->count_connectors == 1 && leased->count_crtcs == 1);
   drmModeFreeResources(leased);
   CHECK(drmModeRevokeLease(master, lessee) == 0);
   CHECK(close(lease) == 0);
   drmModeFreeResources(res); drmModeFreePlaneResources(planes);
   puts("KGSL_DISPLAY_LEASE_IDENTITY_PASS");
   CHECK(close(render_fd) == 0 && close(master) == 0 && fd_count() == initial);
   for (unsigned i = 0; i < 128; ++i) {
      master = tu_kgsl_display_open(primary, &device_major, &device_minor); CHECK(master >= 0);
      CHECK(drmIsMaster(master));
      CHECK(drmDropMaster(master) == 0 && !drmIsMaster(master));
      CHECK(drmSetMaster(master) == 0 && drmIsMaster(master));
      CHECK(close(master) == 0 && fd_count() == initial);
   }
   puts("KGSL_DISPLAY_REACQUIRE_CLEANUP_PASS");
   puts("Virtual DRM descriptor ownership only; no KGSL rendering or panel execution.");
   finish(0);
}
