#ifndef REFRAMED_PICO_SLAM_INPUT_H
#define REFRAMED_PICO_SLAM_INPUT_H

#include "pico-camera.h"
#include <xrt/xrt_tracking.h>

struct pico_slam_input;
struct pico_slam_input_config {
    size_t count;
    uint8_t camera_ids[4];
    uint32_t width[4], height[4];
    int64_t max_age_ns, max_gap_ns, max_skew_ns;
};

/* Caller serializes all calls, stops producers before destruction and keeps
 * sinks alive. Sinks may retain frame references but must not reenter input.
 * Calibration, camera order and IMU axes/units must match the selected tracker.
 * No calibration, pose estimator or physical camera is created here. */
bool pico_slam_input_create(const struct pico_slam_input_config *config,
                            const struct xrt_slam_sinks *sinks, struct pico_slam_input **input);

/* Start with a strictly newer nonzero generation and an explicit qtimer-ns to
 * CLOCK_MONOTONIC offset, error bound and expiry. Use fresh/reset tracker sinks
 * after sensor restart; this function cannot reset a downstream SLAM estimator. */
bool pico_slam_input_begin(struct pico_slam_input *input, uint64_t generation,
                           int64_t camera_offset_ns, int64_t uncertainty_ns, int64_t expires_ns);

/* Copy a validated snapshot before returning. Keep at most one pending image
 * per camera, discard images outside the skew bound and submit complete sets
 * in configured order once IMU timestamps bracket them. Each set uses its
 * earliest mapped time; source timestamps retain individual qtimer times. A true result means
 * accepted input, not necessarily a submitted set or tracking result. */
bool pico_slam_input_camera(struct pico_slam_input *input, uint64_t generation,
                            const void *snapshot, size_t size, int64_t now_ns);

/* Already calibrated and paired SI vectors in the tracker's IMU coordinates,
 * with CLOCK_MONOTONIC capture time. Nonfinite, stale, duplicate or future data
 * is rejected. Excessive per-stream gaps invalidate this generation. */
bool pico_slam_input_imu(struct pico_slam_input *input, uint64_t generation,
                         const struct xrt_imu_sample *sample, int64_t uncertainty_ns, int64_t now_ns);
void pico_slam_input_stop(struct pico_slam_input *input);
void pico_slam_input_destroy(struct pico_slam_input **input);

#endif
