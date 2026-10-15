#ifndef REFRAMED_QUEST_SLAM_RUNTIME_H
#define REFRAMED_QUEST_SLAM_RUNTIME_H

#include "quest-imu-stream.h"
#include "pico-slam-session.h"

struct quest_slam_runtime;
struct quest_slam_runtime_config {
    struct quest_imu_stream_config sensors;
    struct pico_slam_input_config input;
    struct pico_slam_tracking_config tracking;
    struct pico_slam_backend backend;
    const char *backend_config;
    int64_t imu_uncertainty_ns;
    int64_t camera_offset_ns, camera_uncertainty_ns, clock_expires_ns;
};

/* Reuse the protocol-independent VIT session and pose provider from the Pico
 * pipeline; no SSC or Pico camera interface is opened. Require explicit IMU
 * alignment, calibration/axes matching VIT and a verified camera clock mapping.
 * sensors callbacks/userdata must be NULL. Temperature tables, backend library
 * and backend configuration remain available until destruction. */
bool quest_slam_runtime_prepare(const struct quest_slam_runtime_config *config,
                                struct quest_slam_runtime **runtime);
/* One attempt opens only the copied FIFO. The caller must already own and
 * configure the MCU, detect restart/suspend, and stop this owner on either.
 * This neither opens the powerstate node nor starts a camera. */
bool quest_slam_runtime_start(struct quest_slam_runtime *runtime);
/* Serialized consuming thread: poll regularly even when cameras are silent.
 * 0 waits for aligned IMU, 1 means estimator started, negative means failure
 * (-EAGAIN before explicit start). Backend input must be nonblocking. */
int quest_slam_runtime_poll(struct quest_slam_runtime *runtime, size_t *poses);
/* Synchronized mono8 pixels, exact VIT camera order/intrinsics, and exposure
 * center time in the configured camera domain. Pixels are copied before return.
 * No decoding of Pico snapshots, inferred exposure time or axis rotation. */
bool quest_slam_runtime_camera(struct quest_slam_runtime *runtime, uint64_t generation,
                               const struct pico_slam_mono8_frame *frame);
struct pico_slam_tracking *quest_slam_runtime_tracking(struct quest_slam_runtime *runtime);
void quest_slam_runtime_stop(struct quest_slam_runtime *runtime);
/* Stop capture callbacks and release HMD references before destroying. */
void quest_slam_runtime_destroy(struct quest_slam_runtime **runtime);

#endif
