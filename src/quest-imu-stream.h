#ifndef REFRAMED_QUEST_IMU_STREAM_H
#define REFRAMED_QUEST_IMU_STREAM_H

#include "quest-imu.h"

struct quest_imu_stream;
struct quest_imu_stream_config {
    struct quest_imu_config imu;
    uint64_t generation;
    int64_t max_gap_ns;
    void *userdata;
    bool (*sample)(void *userdata, uint64_t generation, const struct quest_imu_sample *sample);
    void (*invalidated)(void *userdata, uint64_t generation, int error);
};

/* Explicit native acquisition: opens only /dev/syncboss_stream0, installs the
 * packet-80 filter and clears that descriptor's queue. Does not start/configure
 * the MCU or a direct channel. The caller establishes exclusive sensor ownership
 * and verified calibration/scales first; offset tables stay alive and immutable.
 * Calls and callbacks are serialized; callbacks must not reenter the owner. */
bool quest_imu_stream_open(const struct quest_imu_stream_config *config,
                           struct quest_imu_stream **stream);

/* Nonblocking, at most eight 4096-byte reads. Uses fresh CLOCK_MONOTONIC checks
 * even on empty input. Returns emitted samples or a negative errno. Malformed
 * batches, gaps, clock regressions and read/sink failures invalidate and close
 * the generation. A batch is validated before any of its samples are emitted.
 * A stopped generation never resumes; destroy it and explicitly open a new one. */
int quest_imu_stream_poll(struct quest_imu_stream *stream);
/* Call on suspend, MCU restart or ownership loss, before accepting more poses. */
void quest_imu_stream_stop(struct quest_imu_stream *stream);
void quest_imu_stream_destroy(struct quest_imu_stream **stream);

#endif
