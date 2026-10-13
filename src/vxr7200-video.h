#ifndef REFRAMED_VXR7200_VIDEO_H
#define REFRAMED_VXR7200_VIDEO_H

#include "vxr7200-transport.h"

#include <stdbool.h>

struct vxr7200_video_status {
    uint16_t horizontal_active, vertical_active;
    uint16_t horizontal_total, vertical_total;
    uint32_t frame_ticks;
    uint16_t clock_multiplier;
    uint8_t clock_divider;
    bool stock_dimensions;
    /* Hz = numerator / frame_ticks when rate_valid. No kernel floating point. */
    uint64_t rate_numerator;
    bool rate_valid;
};

/* Read the five stock timing registers twice; return -EAGAIN if they changed.
 * This does not provide an atomic snapshot, input-lock proof or a mux-switch
 * decision. stock_dimensions means only the stock allowlist matched. Zero or
 * unknown dimensions are reported, not treated as I2C failures. The caller
 * owns power/reset and serializes the entire operation against other bridge
 * commands. No writes, retries, power changes or delays are performed.
 * Failure leaves status unchanged. */
int vxr7200_read_video_status(const struct vxr7200_bus *bus,
                             struct vxr7200_video_status *status);

#endif
