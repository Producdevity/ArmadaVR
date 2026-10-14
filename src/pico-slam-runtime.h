#ifndef REFRAMED_PICO_SLAM_RUNTIME_H
#define REFRAMED_PICO_SLAM_RUNTIME_H

#include "pico-slam-session.h"
#include "pico-slam-imu.h"
#include "ssc-imu-session.h"

struct pico_slam_runtime;
struct pico_slam_runtime_config {
    struct ssc_imu_session_config sensors;
    struct pico_slam_imu_config imu;
    struct pico_slam_input_config input;
    struct pico_slam_tracking_config tracking;
    struct pico_slam_backend backend;
    const char *backend_config;
    uint64_t generation;
    int64_t camera_offset_ns, camera_uncertainty_ns, clock_expires_ns;
};

/* Explicit opt-in: start the selected SSC raw/bias subscriptions, then start VIT
 * on the consuming thread after its first paired IMU sample. Sensor callbacks
 * must be NULL; sensors.data remains the clock-calibration callback argument.
 * The backend, calibration data and configured camera clock must remain valid.
 * One owner serializes poll/captured/stop/destroy. HMD readers may borrow the
 * independently locked provider; release the HMD before destroying this owner.
 * No camera, link, DMA buffer or device configuration is created here. */
bool pico_slam_runtime_create(const struct pico_slam_runtime_config *config,
                              struct pico_slam_runtime **runtime);
/* Allocate the queue and estimator interface without subscribing to SSC. This
 * permits camera configuration before live samples can fill the bounded queue.
 * Start performs the sensor-session validation and is attempted once. */
bool pico_slam_runtime_prepare(const struct pico_slam_runtime_config *config,
                               struct pico_slam_runtime **runtime);
bool pico_slam_runtime_start(struct pico_slam_runtime *runtime);

/* 0: waiting for paired IMU, 1: estimator running, negative errno: stopped.
 * Poll regularly, even without frames. Drain at most 128 IMU pairs and 32 poses.
 * Start camera delivery only after 1; its first frame must meet the session gap
 * bound. SSC/clock/input/backend failures end this owner, requiring recreation.
 * Failed worker callbacks invalidate HMD poses immediately, without calling VIT. */
int pico_slam_runtime_poll(struct pico_slam_runtime *runtime, int64_t now_ns, size_t *poses);
bool pico_slam_runtime_captured(struct pico_slam_runtime *runtime, uint64_t generation,
                                const struct pico_camera_capture_frame *frame, int64_t now_ns);
struct pico_slam_tracking *pico_slam_runtime_tracking(struct pico_slam_runtime *runtime);
void pico_slam_runtime_get_status(struct pico_slam_runtime *runtime, struct ssc_imu_session_status *status);
void pico_slam_runtime_stop(struct pico_slam_runtime *runtime);
/* Join callbacks before freeing the queue and estimator. Cannot run on worker. */
bool pico_slam_runtime_destroy(struct pico_slam_runtime **runtime);

#endif
