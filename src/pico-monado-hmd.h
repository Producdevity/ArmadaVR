#ifndef REFRAMED_PICO_MONADO_HMD_H
#define REFRAMED_PICO_MONADO_HMD_H

#include "pico-head-session.h"
#include <xrt/xrt_device.h>

struct pico_hmd_view {
    struct xrt_view display;
    struct xrt_fov fov;
    struct xrt_pose eye_pose;
    unsigned grid_width, grid_height;
    const struct xrt_uv_triplet *distortion;
};

struct pico_hmd_config {
    const char *serial;
    unsigned screen_width, screen_height;
    int64_t frame_interval_ns;
    struct pico_hmd_view views[2];
    struct ssc_imu_session_config sensors;
    struct pico_head_tracking_config tracking;
};

/* Optics are explicit, immutable calibration inputs. Each row-major grid maps
 * normalized per-eye screen coordinates to RGB render-texture coordinates.
 * The factory copies the grids; sensor clock callback data must outlive the
 * device. Device destruction stops and joins its sensor session. */
bool pico_monado_hmd_create(const struct pico_hmd_config *config, struct xrt_device **device);

#endif
