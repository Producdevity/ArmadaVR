#ifndef REFRAMED_PICO_SLAM_IMU_H
#define REFRAMED_PICO_SLAM_IMU_H

#include "ssc-calibration.h"
#include <xrt/xrt_tracking.h>

struct pico_slam_imu;
struct pico_slam_imu_config {
    float sensor_to_imu[9];
    double acceleration_scale, angular_velocity_scale;
    int64_t max_age_ns, max_gap_ns;
    unsigned minimum_accuracy;
};

/* Bias-corrected SSC worker to serialized SLAM-owner handoff. Supply measured
 * axes/units matching the estimator calibration; no sensors or threads start.
 * One producer and one consumer may run concurrently. Stop/join both before
 * destruction. All methods except destruction lock only local queue state. */
bool pico_slam_imu_create(const struct pico_slam_imu_config *config, struct pico_slam_imu **imu);
bool pico_slam_imu_begin(struct pico_slam_imu *imu, uint64_t generation, int64_t now_ns);

/* Pair equal SSC ticks AND mapped timestamps, never interpolate. Older unmatched
 * samples may be discarded. Retain at most 32 samples per sensor and 128 pairs.
 * Bad current-generation data, gaps or exhaustion end the generation. Stale
 * callbacks cannot invalidate a newer generation. No estimator calls here. */
bool pico_slam_imu_push(struct pico_slam_imu *imu, const struct ssc_bias_sample *sample, int64_t now_ns);

/* 1: copied pair, 0: empty, negative errno: invalid/inactive. Output is unchanged
 * unless a pair is returned. Call regularly to expire silent/stale streams.
 * The owner must stop the SLAM session on failure before publishing more poses. */
int pico_slam_imu_pop(struct pico_slam_imu *imu, uint64_t generation, int64_t now_ns,
                      struct xrt_imu_sample *sample, int64_t *uncertainty_ns);
void pico_slam_imu_stop(struct pico_slam_imu *imu, uint64_t generation);
void pico_slam_imu_destroy(struct pico_slam_imu **imu);

#endif
