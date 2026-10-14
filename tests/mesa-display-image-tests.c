/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
struct wsi_display { int fd; };
#include "mesa-display-image-types.h"
#define CHECK(expr) do { if (!(expr)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(2); \
} } while (0)
enum fault { NONE, CREATE, IMPORT, FRAMEBUFFER };
static enum fault fault;
static unsigned planes, creates, destroys, imports, fb_adds, fb_dels, closes, bad_closes;
static bool handle_live, fb_live, use_modifier;
static int backing = -1;
static VkResult wsi_create_image(struct wsi_swapchain *chain, int *info, struct wsi_image *image)
{
   CHECK(info == &chain->image_info); ++creates;
   if (fault == CREATE) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   backing = memfd_create("display-test", MFD_CLOEXEC); CHECK(backing >= 0);
   CHECK(ftruncate(backing, 65536) == 0);
   *image = (struct wsi_image){.num_planes=planes, .dma_buf_fd=backing,
                             .drm_modifier=use_modifier ? 0x1234 : DRM_FORMAT_MOD_INVALID};
   for (unsigned i=0; i<planes; ++i) { image->row_pitches[i]=256; image->offsets[i]=i*4096; }
   return VK_SUCCESS;
}
static void wsi_destroy_image(struct wsi_swapchain *chain, struct wsi_image *image)
{
   (void)chain; ++destroys; CHECK(!fb_live && !handle_live);
   CHECK(image->dma_buf_fd == backing && close(backing) == 0); backing=-1;
}
static int import_buffer(int fd, int dma_fd, uint32_t *handle)
{
   CHECK(fd==91 && dma_fd==backing && fcntl(dma_fd,F_GETFD)>=0); ++imports;
   if (fault==IMPORT) { errno=ENOMEM; return -1; }
   *handle=17; handle_live=true; return 0;
}
static int add_framebuffer(int fd, uint32_t width, uint32_t height, uint32_t format,
                          const uint32_t handles[4], const uint32_t pitches[4],
                          const uint32_t offsets[4], const uint64_t modifiers[4], uint32_t *fb, uint32_t flags)
{
   CHECK(fd==91 && width==64 && height==64 && format==DRM_FORMAT_XRGB8888); ++fb_adds;
   CHECK(handle_live && !fb_live && (modifiers!=NULL)==use_modifier);
   CHECK(flags==(use_modifier ? DRM_MODE_FB_MODIFIERS : 0));
   for (unsigned i=0; i<4; ++i) {
      CHECK(handles[i]==(i<planes ? 17u : 0));
      if (i<planes) {
         CHECK(pitches[i]==256 && offsets[i]==i*4096);
         if (modifiers) CHECK(modifiers[i]==0x1234);
      }
   }
   if (fault==FRAMEBUFFER) { errno=EINVAL; return -1; }
   *fb=29; fb_live=true; return 0;
}
static int remove_framebuffer(int fd, uint32_t fb)
{
   CHECK(fd==91 && fb==29 && fb_live); ++fb_dels; fb_live=false; return 0;
}
static int close_handle(int fd, unsigned long request, void *arg)
{
   CHECK(fd==91 && request==DRM_IOCTL_GEM_CLOSE && !fb_live);
   CHECK(((struct drm_gem_close *)arg)->handle==17); ++closes;
   if (!handle_live) { ++bad_closes; errno=ENOENT; return -1; }
   handle_live=false; return 0;
}
#define drmPrimeFDToHandle import_buffer
#define drmModeAddFB2WithModifiers add_framebuffer
#define drmModeRmFB remove_framebuffer
#define drmIoctl close_handle
#include "display-image-under-test.h"
int main(void)
{
   struct wsi_display display={91};
   struct wsi_display_swapchain chain={.wsi=&display};
   VkSwapchainCreateInfoKHR info={.imageExtent={64,64}};
   unsigned cases=0, failures=0;
   for (unsigned repeat=0; repeat<128; ++repeat) {
      for (planes=1; planes<=4; ++planes) {
         for (unsigned modifier=0; modifier<2; ++modifier) {
            use_modifier=modifier;
            for (fault=NONE; fault<=FRAMEBUFFER; ++fault) {
               ++cases; creates=destroys=imports=fb_adds=fb_dels=closes=bad_closes=0;
               handle_live=fb_live=false;
               struct wsi_display_image image={0};
               VkResult result=wsi_display_image_init(&chain.base,&info,DRM_FORMAT_XRGB8888,&image);
               if (fault==NONE) {
                  CHECK(result==VK_SUCCESS && image.chain==&chain && image.state==WSI_IMAGE_IDLE);
                  wsi_display_image_finish(&chain.base,&image);
               } else CHECK(result==(fault==CREATE ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_OUT_OF_HOST_MEMORY));
               bool allocated=fault!=CREATE, imported=allocated && fault!=IMPORT;
               bool valid=creates==1 && destroys==allocated && imports==allocated &&
                          closes==imported && bad_closes==0 && fb_adds==imported &&
                          fb_dels==(fault==NONE) && !handle_live && !fb_live && backing==-1;
               if (!valid) ++failures;
            }
         }
      }
   }
   printf("Display image: %u cases, %u PRIME ownership failures; modeled DRM, real descriptors\n",cases,failures);
   return failures ? 1 : 0;
}
