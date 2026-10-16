#ifndef REFRAMED_QUEST_CAMERA_PIXELS_H
#define REFRAMED_QUEST_CAMERA_PIXELS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum quest_camera_pixel_format {
    QUEST_CAMERA_MONO8 = 1,
    QUEST_CAMERA_MIPI_RAW10 = 2,
};
enum quest_camera_pixel_curve {
    QUEST_CAMERA_CURVE_COPY,
    QUEST_CAMERA_CURVE_POINT5,
    QUEST_CAMERA_CURVE_POINT5_OFFSET,
};
struct quest_camera_pixel_layout {
    uint32_t width, height, first_row;
    size_t stride;
    enum quest_camera_pixel_format format;
    enum quest_camera_pixel_curve curve;
};

/* Convert stable, CPU-readable pixels to contiguous mono8. The caller supplies
 * the verified active layout, including whether to skip a leading metadata row
 * (first_row 0 or 1). No format or gamma is inferred from metadata/config files.
 * RAW10 uses MIPI groups of four pixels in five bytes. COPY drops its low bits;
 * POINT5 curves reproduce the stock CPU approximations, not a power function.
 * MONO8 accepts COPY only. No geometry/calibration or exposure-time adjustment.
 * Output size must equal width*height, and the buffers must not overlap.
 * Failure preserves the output. DMA-BUF ownership/cache handling is external. */
bool quest_camera_pixels_mono8(const void *source, size_t source_size,
                               const struct quest_camera_pixel_layout *layout,
                               void *destination, size_t destination_size);
#endif
