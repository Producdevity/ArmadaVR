#ifndef REFRAMED_PICO_SLAM_TRACKING_H
#define REFRAMED_PICO_SLAM_TRACKING_H

#include <xrt/xrt_defines.h>

struct pico_slam_tracking;
struct pico_slam_tracking_config {
    /* T_openxr_slam and T_imu_head. No headset-specific defaults are inferred. */
    struct xrt_pose openxr_from_slam, imu_from_head;
    int64_t max_age_ns, max_gap_ns, max_prediction_ns;
};

bool pico_slam_tracking_create(const struct pico_slam_tracking_config *config,
                               struct pico_slam_tracking **tracking);
/* The estimator must be recreated/reset before beginning a newer generation. */
bool pico_slam_tracking_begin(struct pico_slam_tracking *tracking, uint64_t generation);
/* T_slam_imu, metres, CLOCK_MONOTONIC capture time. A malformed, regressing or
 * discontinuous estimate invalidates this generation. Old-generation callbacks
 * are ignored. There is no automatic fallback between unrelated world frames. */
bool pico_slam_tracking_push(struct pico_slam_tracking *tracking, uint64_t generation,
                             const struct xrt_pose *slam_from_imu, int64_t timestamp_ns,
                             int64_t uncertainty_ns, int64_t now_ns);
bool pico_slam_tracking_get(struct pico_slam_tracking *tracking, int64_t now_ns,
                            int64_t at_ns, struct xrt_space_relation *relation,
                            int64_t *sample_time_ns);
void pico_slam_tracking_stop(struct pico_slam_tracking *tracking);
/* Stop producers and release HMD references before destroying this object. */
void pico_slam_tracking_destroy(struct pico_slam_tracking **tracking);

#endif
