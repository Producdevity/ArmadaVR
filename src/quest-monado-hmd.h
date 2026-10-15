#ifndef REFRAMED_QUEST_MONADO_HMD_H
#define REFRAMED_QUEST_MONADO_HMD_H

#include "calibrated-hmd.h"

struct quest_slam_runtime;

struct quest_hmd_config {
    const char *serial;
    unsigned screen_width, screen_height;
    int64_t frame_interval_ns;
    struct calibrated_hmd_view views[2];
    struct quest_slam_runtime *tracking;
};

/* Requires explicit per-eye optics; the Pico polynomial model is refused.
 * Copies the grids and borrows tracking. Stop capture and destroy the HMD
 * before freeing tracking. Creation never starts an IMU or camera session. */
bool quest_monado_hmd_create(const struct quest_hmd_config *config, struct xrt_device **device);

#endif
