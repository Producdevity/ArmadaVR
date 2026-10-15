#ifndef REFRAMED_PICO_MONADO_HMD_H
#define REFRAMED_PICO_MONADO_HMD_H

#include "pico-head-session.h"
#include "pico-slam-tracking.h"
#include "calibrated-hmd.h"

struct pico_hmd_config {
    const char *serial;
    unsigned screen_width, screen_height;
    int64_t frame_interval_ns;
    struct calibrated_hmd_view views[2];
    struct ssc_imu_session_config sensors;
    struct pico_head_tracking_config tracking;
    struct pico_slam_tracking *visual_tracking;
};

/* Optics are explicit, immutable calibration inputs. Each row-major grid maps
 * normalized per-eye screen coordinates to RGB render-texture coordinates.
 * The factory copies the grids; sensor clock callback data must outlive the
 * device. Device destruction stops and joins its sensor session.
 * With visual_tracking supplied, the HMD borrows that thread-safe provider and
 * does not create a second IMU session. Its owner must stop capture and destroy
 * the HMD before freeing the provider. No orientation-only fallback is applied. */
bool pico_monado_hmd_create(const struct pico_hmd_config *config, struct xrt_device **device);

#endif
