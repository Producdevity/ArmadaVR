#ifndef REFRAMED_PICO_SLAM_SESSION_H
#define REFRAMED_PICO_SLAM_SESSION_H

#include "pico-slam-input.h"
#include "pico-camera-capture.h"
#include "pico-slam-tracking.h"
#include <vit/vit_interface.h>

struct pico_slam_session;
struct pico_slam_backend {
    PFN_vit_api_get_version version;
    PFN_vit_tracker_create create;
    PFN_vit_tracker_destroy destroy;
    PFN_vit_tracker_has_image_format has_format;
    PFN_vit_tracker_start start;
    PFN_vit_tracker_stop stop;
    PFN_vit_tracker_push_imu_sample imu;
    PFN_vit_tracker_push_img_sample image;
    PFN_vit_tracker_pop_pose pop;
    PFN_vit_pose_get_data pose_data;
    PFN_vit_pose_destroy pose_destroy;
};

/* The backend and its config/calibration must remain available throughout this
 * object's life. Use the patched Basalt with nonblocking-input=1 and
 * deterministic=0. Capture calls and poll/stop/begin are serialized by the
 * caller. HMD pose reads use the independently locked tracking provider.
 * No sensors, threads or devices are started by this interface. */
bool pico_slam_session_create(const struct pico_slam_input_config *input,
                              const struct pico_slam_tracking_config *tracking,
                              const struct pico_slam_backend *backend, const char *backend_config,
                              struct pico_slam_session **session);

/* Recreate the estimator for every strictly newer generation. A failed start
 * leaves tracking invalid; retry requires another generation. Camera times are
 * mapped from qtimer, IMU times already use CLOCK_MONOTONIC. */
bool pico_slam_session_begin(struct pico_slam_session *session, uint64_t generation,
                             int64_t camera_offset_ns, int64_t uncertainty_ns,
                             int64_t expires_ns, int64_t now_ns);
bool pico_slam_session_camera(struct pico_slam_session *session, uint64_t generation,
                              const void *snapshot, size_t size, int64_t now_ns);
bool pico_slam_session_mono8(struct pico_slam_session *session, uint64_t generation,
                             const struct pico_slam_mono8_frame *frame, int64_t now_ns);
/* Called only from the capture owner's frame callback, while CPU read access
 * is synchronized. The input adapter copies pixels before this returns. */
bool pico_slam_session_captured(struct pico_slam_session *session, uint64_t generation,
                                const struct pico_camera_capture_frame *frame, int64_t now_ns);
bool pico_slam_session_imu(struct pico_slam_session *session, uint64_t generation,
                           const struct xrt_imu_sample *sample, int64_t uncertainty_ns, int64_t now_ns);
/* Call regularly even when capture is silent. Drain at most 32 poses per call.
 * Capture gaps, expiry, backend failures or bad poses invalidate tracking.
 * Queue exhaustion ends the generation; input is never retried into it. */
bool pico_slam_session_poll(struct pico_slam_session *session, uint64_t generation,
                            int64_t now_ns, size_t *poses);
struct pico_slam_tracking *pico_slam_session_tracking(struct pico_slam_session *session);
void pico_slam_session_stop(struct pico_slam_session *session);
/* Stop capture callbacks and release the HMD before destroying the session. */
void pico_slam_session_destroy(struct pico_slam_session **session);

#endif
