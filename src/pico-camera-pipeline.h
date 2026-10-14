#ifndef REFRAMED_PICO_CAMERA_PIPELINE_H
#define REFRAMED_PICO_CAMERA_PIPELINE_H
#include "pico-camera-control.h"

struct pico_camera_pipeline;
struct pico_camera_pipeline_device { int fd; int32_t handle; };
struct pico_camera_pipeline_config {
    int crm_fd;
    int32_t session;
    size_t count;
    struct {
        int32_t link;
        struct pico_camera_pipeline_device ife, phy, sensor;
    } cameras[4];
};

/* Devices must already be acquired/configured, with valid IFE init packets and
 * sensor stream-on/off tables. Links contain sensor/IFE, not PHY. Sensor pairing
 * and synchronization must be configured separately. All handles/fds are borrowed.
 * This owner opens no device, selects no mode and never probes a sensor. */
int pico_camera_pipeline_create(const struct pico_camera_pipeline_config *config,
                                 struct pico_camera_pipeline **pipeline);
/* One attempt per owner: activate links, start IFEs, PHYs, then sensors. A failed
 * call may have changed hardware; stop must include that attempted device. */
int pico_camera_pipeline_start(struct pico_camera_pipeline *pipeline);
/* Stop order is IFE, sensor, PHY, then link deactivation, as in retained CamX.
 * Attempts all affected devices even after errors. Failed stops are not retried
 * automatically. Success records ioctl acceptance, not proof of DMA quiescence.
 * The caller must stop scheduling/events and independently verify quiescence
 * before releasing request buffers, handles or descriptors. */
int pico_camera_pipeline_stop(struct pico_camera_pipeline *pipeline);
/* Refuses destruction after an uncertain stop. Does not release kernel handles. */
int pico_camera_pipeline_destroy(struct pico_camera_pipeline **pipeline);
#endif
