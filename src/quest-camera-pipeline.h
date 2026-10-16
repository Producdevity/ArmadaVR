#ifndef REFRAMED_QUEST_CAMERA_PIPELINE_H
#define REFRAMED_QUEST_CAMERA_PIPELINE_H
#include "quest-camera-control.h"

struct quest_camera_pipeline;
struct quest_camera_pipeline_device { int fd; int32_t handle; };
struct quest_camera_pipeline_config {
    int crm_fd;
    int32_t session;
    size_t count;
    struct {
        int32_t link;
        struct quest_camera_pipeline_device ife, phy, sensor;
    } cameras[4];
};

/* Devices must already be acquired/configured, with valid IFE init packets and
 * sensor stream tables. Links and local sensor/IFE/PHY membership must already
 * match the selected Quest mode. MCU camera control and hardware synchronization
 * remain separate. Remote/SFE or partial hardware-stop modes are excluded. Pairing
 * and synchronization must be configured separately. All handles/fds are borrowed.
 * This owner opens no device, selects no mode and never probes a sensor. */
int quest_camera_pipeline_create(const struct quest_camera_pipeline_config *config,
                                 struct quest_camera_pipeline **pipeline);
/* One attempt per owner: activate links, start IFEs, PHYs, then sensors. A failed
 * call may have changed hardware; stop must include that attempted device. */
int quest_camera_pipeline_start(struct quest_camera_pipeline *pipeline);
/* Stop order is IFE, sensor, PHY, then link deactivation, as in retained CamX.
 * Attempts all affected devices even after errors. Failed stops are not retried
 * automatically. Success records ioctl acceptance, not proof of DMA quiescence.
 * The caller must stop scheduling/events and independently verify quiescence
 * before releasing request buffers, handles or descriptors. */
int quest_camera_pipeline_stop(struct quest_camera_pipeline *pipeline);
/* Call only after independently established DMA/worker quiescence and returned
 * control calls. Stop acceptance alone is insufficient. No device operation. */
int quest_camera_pipeline_quiesced(struct quest_camera_pipeline *pipeline);
/* Refuses destruction before quiescence or after an uncertain stop. No handles
 * are released here. A failed stop is never retried. */
int quest_camera_pipeline_destroy(struct quest_camera_pipeline **pipeline);
/* Local disposal for uncertain cleanup, only after the caller has independently
 * quiesced hardware and closed every borrowed descriptor. */
void quest_camera_pipeline_closed(struct quest_camera_pipeline **pipeline);
#endif
