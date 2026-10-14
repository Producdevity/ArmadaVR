#ifndef REFRAMED_PICO_CAMERA_RIG_H
#define REFRAMED_PICO_CAMERA_RIG_H
#include "pico-camera-session.h"
#include "pico-camera-sensor-setup.h"
#include "pico-camera-front-end.h"

struct pico_camera_rig;
struct pico_camera_rig_config {
    struct pico_camera_session_config session;
    enum pico_camera_memory_mapping command_mapping;
    struct {
        struct pico_camera_sensor_table initialize, mode, synchronize, stream_on, stream_off;
        struct pico_camera_csiphy_config phy;
        struct pico_camera_ife_clocks clocks;
        struct pico_camera_ife_bandwidth bandwidth;
        uint32_t sample_pattern;
        bool configure_throttle;
        float utilization;
    } cameras[2];
};

/* Acquire and configure an explicitly selected stereo mono8 pair. Sensors must
 * already be probed and physical settings verified by the caller. All tables
 * are copied before return; descriptors remain borrowed. This owns the session,
 * sensor/IFE/PHY command buffers and stream-control state. It opens no node and
 * starts no streaming. Failure may still return an owner requiring cleanup. */
int pico_camera_rig_prepare(const struct pico_camera_rig_config *config,
                               struct pico_camera_rig **device);
/* Borrow handles only after complete configuration, before stop. */
bool pico_camera_rig_view(const struct pico_camera_rig *device,
                             struct pico_camera_session_view *view);
/* Queue initial capture requests and establish estimator/clock readiness first.
 * Start is attempted once; a failed attempt must also be stopped. */
int pico_camera_rig_start(struct pico_camera_rig *device);
/* Stop scheduling/callbacks before calling. Attempts all affected stream stops.
 * Success is ioctl acceptance, not proof of DMA quiescence. */
int pico_camera_rig_stop(struct pico_camera_rig *device);
/* Requires independent quiescence, returned controls and destruction of capture
 * transports/other children borrowing the session. No device operation retries. */
int pico_camera_rig_quiesced(struct pico_camera_rig *device);
/* Release commands before hardware/session handles. Uncertain kernel cleanup
 * remains owned; it cannot be resolved by simply calling release again. */
int pico_camera_rig_release(struct pico_camera_rig **device);
#endif
