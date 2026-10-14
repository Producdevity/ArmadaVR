#ifndef REFRAMED_PICO_CAMERA_SESSION_H
#define REFRAMED_PICO_CAMERA_SESSION_H
#include "pico-camera-control.h"
#include "pico-camera-isp.h"

struct pico_camera_session;
struct pico_camera_session_config {
    int crm_fd, ife_fd;
    unsigned master;
    struct {
        int sensor_fd, phy_fd;
        uint32_t sensor_slot, phy_version;
        struct pico_camera_ife_config input;
    } cameras[2];
};
struct pico_camera_session_view {
    int32_t session;
    struct pico_camera_ife_capabilities capabilities;
    struct {
        int32_t sensor, phy, ife, link;
        struct pico_camera_ife_hardware hardware;
    } cameras[2];
};

/* Acquire two explicitly selected, already probed sensors, PHYs and RDI0 inputs
 * in one CRM session, then link and synchronize them. Queries must match sensor
 * slots, PHY routing and version before acquisition starts. Descriptors are
 * borrowed exclusively for this owner and its children. No nodes are opened,
 * no sensor is probed/configured/started and no wiring is inferred.
 * A nonnull owner must be retained even after failure. Calls are serialized. */
int pico_camera_session_acquire(const struct pico_camera_session_config *config,
                                struct pico_camera_session **session);
/* The view and all handles borrow ownership until release/closed. */
bool pico_camera_session_view(const struct pico_camera_session *session,
                              struct pico_camera_session_view *view);
/* Requires stopped scheduling/events, returned controls, independent device/DMA
 * quiescence and release of all children borrowing the handles. */
int pico_camera_session_quiesced(struct pico_camera_session *session);
/* One cleanup attempt. Failed releases or missing acquisition handles remain
 * uncertain; this never retries them or destroys their enclosing CRM session. */
int pico_camera_session_release(struct pico_camera_session **session);
/* Local destruction only, after the caller has closed every borrowed descriptor
 * and independently established quiescence. This is required for uncertain
 * kernel cleanup; it does not itself close descriptors or stop hardware. */
void pico_camera_session_closed(struct pico_camera_session **session);
#endif
