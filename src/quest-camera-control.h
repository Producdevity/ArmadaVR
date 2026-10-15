#ifndef REFRAMED_QUEST_CAMERA_CONTROL_H
#define REFRAMED_QUEST_CAMERA_CONTROL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct quest_camera_sensor_capabilities {
    uint32_t slot, secure, pitch, roll, yaw;
    uint32_t actuator_slot, eeprom_slot, ois_slot, flash_slot, phy_slot;
};
struct quest_camera_phy_capabilities { uint32_t slot, version, clock_lane; };
struct quest_camera_isp_capabilities {
    int32_t device_mmu, cdm_mmu;
    uint32_t device_count;
    struct { uint32_t type, count, major, minor, increment; } devices[8];
};

/* Meta dbc2bc83 camera UAPI. The owner supplies already identified descriptors;
 * opening or closing CRM has global side effects. These queries neither acquire
 * nor start a sensor, retry ioctls, or replace output after failure. */
int quest_camera_sensor_query(int fd, struct quest_camera_sensor_capabilities *caps);
int quest_camera_phy_query(int fd, struct quest_camera_phy_capabilities *caps);
int quest_camera_isp_query(int fd, struct quest_camera_isp_capabilities *caps);

/* Mutating CRM transport for an exclusively owned, already identified fd.
 * No open, retry, cleanup or sensor discovery is implicit. Positive handles
 * survive failed copy-back; other failed commands preserve the input handles.
 * Even a failure without a returned handle can have changed kernel state.
 * The owner must stop and establish quiescence before reuse. Flush is not proof
 * of DMA quiescence. V1 links support 2-8 already acquired devices, excluding PHY.
 * Remote sensors and trigger-mode V3 links are not supported here. */
int quest_camera_session_create(int fd, int32_t *session);
int quest_camera_session_destroy(int fd, int32_t *session);
int quest_camera_link_create(int fd, int32_t session, const int32_t *devices, size_t count, int32_t *link);
int quest_camera_link_destroy(int fd, int32_t session, int32_t *link);
int quest_camera_link_activate(int fd, int32_t session, int32_t link, bool active);
int quest_camera_links_synchronize(int fd, int32_t session, const int32_t links[2], int32_t master);
int quest_camera_request_schedule(int fd, int32_t session, int32_t link, uint64_t request,
                                  bool synchronized, bool bubble_recovery);
int quest_camera_requests_flush(int fd, int32_t session, int32_t link);

#endif
