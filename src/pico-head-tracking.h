#ifndef REFRAMED_PICO_HEAD_TRACKING_H
#define REFRAMED_PICO_HEAD_TRACKING_H

#include "ssc-imu.h"
#include <xrt/xrt_defines.h>

struct pico_head_tracking;

struct pico_head_tracking_config {
    float sensor_to_head[9];
    double acceleration_scale, angular_velocity_scale;
    int64_t max_age_ns, max_gap_ns;
    unsigned minimum_accuracy;
};

/* Explicit proper rotation and positive unit scales map validated IMU vectors
 * into Monado's head coordinates, m/s^2 and rad/s. Calibration must already be
 * applied by the caller; no device axes, units or bias are guessed here.
 * Lifecycle calls must not race destruction. Push/get are serialized internally.
 * Output must initially be NULL. */
bool pico_head_tracking_create(const struct pico_head_tracking_config *config,
                               struct pico_head_tracking **tracking);
bool pico_head_tracking_begin(struct pico_head_tracking *tracking, uint64_t generation);

/* Pair exact capture ticks/timestamps using bounded queues. Events must belong
 * to the current subscription generation and CLOCK_MONOTONIC calibration.
 * A true result accepts an event, not necessarily a complete pair or pose.
 * An excessive gap between pairs invalidates tracking until a newer begin. */
bool pico_head_tracking_push(struct pico_head_tracking *tracking,
                             const struct ssc_imu_event *event, int64_t now_ns);

/* Returns orientation only at sample_time_ns, without prediction or position.
 * Invalid, incomplete, future or stale data clears both outputs. */
bool pico_head_tracking_get(struct pico_head_tracking *tracking, int64_t now_ns,
                            struct xrt_space_relation *relation, int64_t *sample_time_ns);
#define PICO_HEAD_MAX_PREDICTION_NS INT64_C(50000000)

/* Resolve CLOCK_MONOTONIC query times from this stream's history. Forward
 * prediction includes timestamp uncertainty in the 50 ms limit; older-than-
 * history, stale and invalid requests clear the output. No position is inferred.
 * Angular velocity is in tracking-origin coordinates, as required by Monado. */
bool pico_head_tracking_get_at(struct pico_head_tracking *tracking, int64_t now_ns,
                               int64_t at_ns, struct xrt_space_relation *relation);
void pico_head_tracking_disconnect(struct pico_head_tracking *tracking);
void pico_head_tracking_destroy(struct pico_head_tracking **tracking);

#endif
