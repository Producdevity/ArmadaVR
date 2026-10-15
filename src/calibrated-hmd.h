#ifndef REFRAMED_CALIBRATED_HMD_H
#define REFRAMED_CALIBRATED_HMD_H

#include <xrt/xrt_device.h>

struct calibrated_hmd_radial {
    float tangent_scale[2], tangent_offset[2];
    float radial[11], chromatic[6];
};

struct calibrated_hmd_view {
    struct xrt_view display;
    struct xrt_fov fov;
    struct xrt_pose eye_pose;
    unsigned grid_width, grid_height;
    const struct xrt_uv_triplet *distortion;
    bool radial_enabled;
    struct calibrated_hmd_radial radial;
};

struct calibrated_hmd_config {
    const char *serial, *name, *tracking_name;
    unsigned screen_width, screen_height;
    int64_t frame_interval_ns;
    struct calibrated_hmd_view views[2];
    bool position_tracking;
};

struct calibrated_hmd_provider {
    void *userdata;
    bool (*get_pose)(void *, int64_t now, int64_t at, struct xrt_space_relation *);
    bool (*update_input)(void *, int64_t now, struct xrt_space_relation *, int64_t *sample);
    bool (*release)(void *);
};

/* Copies optics and callbacks. The provider is borrowed until creation succeeds;
 * destruction then calls release when supplied. A failed release keeps the
 * device alive so the owner can finish stopping capture before retrying. */
bool calibrated_hmd_create(const struct calibrated_hmd_config *config,
                           const struct calibrated_hmd_provider *provider,
                           struct xrt_device **device);

#endif
