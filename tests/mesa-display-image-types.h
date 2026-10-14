/* SPDX-License-Identifier: MIT */
#ifndef REFRAMED_TEST_DISPLAY_IMAGE_TYPES_H
#define REFRAMED_TEST_DISPLAY_IMAGE_TYPES_H
#include <vulkan/vulkan.h>
#include <stdint.h>
struct wsi_image {
   unsigned num_planes;
   int dma_buf_fd;
   uint64_t drm_modifier;
   uint32_t row_pitches[4], offsets[4];
};
struct wsi_swapchain { int image_info; };
struct wsi_display_swapchain {
   struct wsi_swapchain base;
   struct wsi_display *wsi;
};
struct wsi_display_image {
   struct wsi_image base;
   uint32_t buffer[4], fb_id;
   struct wsi_display_swapchain *chain;
   int state;
   void *fence;
   uint64_t minimum_ns;
};
enum { WSI_IMAGE_IDLE };
#endif
