#ifndef REFRAMED_PICO_SLAM_LIVE_H
#define REFRAMED_PICO_SLAM_LIVE_H
#include "pico-camera-rig.h"
#include "pico-slam-runtime.h"

struct pico_slam_live;
struct pico_slam_live_config {
    struct pico_camera_rig_config cameras;
    struct pico_slam_runtime_config tracking;
    int sync_fd, ion_fd;
    uint32_t ion_heap, stride[2];
    unsigned queue_depth;
    uint64_t first_request_id;
    int64_t frame_period_ns, capture_timeout_ns;
};

/* Explicit, calibrated stereo camera configuration over exclusively borrowed
 * descriptors. Sensors must already be probed; CRM/sync must be nonblocking.
 * Prepares tracking, configures cameras, subscribes events, then starts SSC.
 * No node is opened. Camera streaming waits for poll's first paired IMU.
 * Failure can return an owner; retain it and follow stop/quiesced/destroy.
 * Calls and callbacks are serialized. Calibration/backend remain borrowed. */
int pico_slam_live_prepare(const struct pico_slam_live_config *config, struct pico_slam_live **live);
/* 0 waiting for IMU, 1 streaming, negative errno permanently stopped.
 * Call regularly even with silent descriptors. Prime requests before starting,
 * copy completed frames to tracking and refill a bounded stereo queue. Offsets
 * are verified SOF-to-exposure-center values for the newly queued requests. */
int pico_slam_live_poll(struct pico_slam_live *live, const int64_t center_offsets_ns[2],
                       int64_t now_ns, size_t *poses);
struct pico_slam_tracking *pico_slam_live_tracking(struct pico_slam_live *live);
/* Stop callbacks, invalidate poses and attempt every affected camera stop once.
 * Success only acknowledges the controls; it does not establish DMA quiescence. */
int pico_slam_live_stop(struct pico_slam_live *live);
/* Caller must independently establish device/DMA quiescence first. Retires
 * requests before unsubscribing events and retiring configuration buffers.
 * Uncertain cleanup remains owned; repeated calls never retry failed controls. */
int pico_slam_live_quiesced(struct pico_slam_live *live);
/* Release any HMD borrowing tracking first. Refuses active/unquiesced owners. */
int pico_slam_live_destroy(struct pico_slam_live **live);
#endif
