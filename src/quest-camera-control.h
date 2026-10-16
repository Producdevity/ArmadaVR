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
 * Remote sensor acquisition is not implemented here. */
int quest_camera_session_create(int fd, int32_t *session);
int quest_camera_session_destroy(int fd, int32_t *session);
int quest_camera_link_create(int fd, int32_t session, const int32_t *devices, size_t count, int32_t *link);
enum quest_camera_link_mode {
    QUEST_CAMERA_LINK_STREAMING = 1,
    QUEST_CAMERA_LINK_TRIGGERED = 2,
};
/* V3 supports 1-8 owned devices and an explicit trigger mode. Independent CRM
 * bypasses normal request-manager scheduling and must match the device setup.
 * Positive output handles survive failure as with the legacy link interface. */
int quest_camera_link_create_v3(int fd, int32_t session, const int32_t *devices, size_t count,
                                enum quest_camera_link_mode mode, bool independent, int32_t *link);
struct quest_camera_sync_link { int32_t link, sensor_id; bool remote; };
/* V2 handles up to 16 links. Sensor IDs and remote flags describe the supplied
 * topology; this does not acquire remote sensors or configure MCU frame sync.
 * The pinned kernel accepts but does not use master/sensor/remote fields in this
 * operation. Disabling synchronization requires master=0; enabling requires a
 * member handle. Hardware master selection must be established separately. */
int quest_camera_links_synchronize_v2(int fd, int32_t session,
                                      const struct quest_camera_sync_link *links, size_t count,
                                      bool enabled, int32_t master);
int quest_camera_link_destroy(int fd, int32_t session, int32_t *link);
int quest_camera_link_activate(int fd, int32_t session, int32_t link, bool active);
int quest_camera_links_synchronize(int fd, int32_t session, const int32_t links[2], int32_t master);
int quest_camera_request_schedule(int fd, int32_t session, int32_t link, uint64_t request,
                                  bool synchronized, bool bubble_recovery);
int quest_camera_requests_flush(int fd, int32_t session, int32_t link);

/* Acquisition can power hardware. Handle output starts at zero and preserves
 * positive handles even after failure. Probe requires a prepared kernel-mapped
 * packet at offset zero. PHY supports DPHY or CPHY with optional same-type
 * combo mode; mixed/remote PHY modes are excluded. ISP uses split acquisition:
 * a separate verified hardware-resource command is required before config.
 * No call retries, opens a node or establishes DMA quiescence. */
int quest_camera_sensor_probe_submit(int fd, int32_t packet);
int quest_camera_sensor_acquire(int fd, int32_t session, int32_t *device);
int quest_camera_phy_acquire(int fd, int32_t session, bool combo, bool cphy, int32_t *device);
int quest_camera_isp_device_acquire(int fd, int32_t session, int32_t *device);
int quest_camera_device_configure(int fd, int32_t session, int32_t device,
                                   int32_t packet, uint64_t offset, uint64_t allocation_size);
int quest_camera_device_stream(int fd, int32_t session, int32_t device, bool start);
int quest_camera_device_release(int fd, int32_t session, int32_t *device);

struct quest_camera_isp_hardware {
    uint32_t id[5], paths[5][3], valid;
};
/* The encoded 216-byte v2 resource must remain alive for this call. A failure
 * may leave hardware acquired even when the output remains unchanged. A flush,
 * successful release or closed descriptor alone does not prove DMA quiescence. */
int quest_camera_isp_hardware_acquire(int fd, int32_t session, int32_t device,
                                     const void *resource, size_t size,
                                     struct quest_camera_isp_hardware *hardware);
int quest_camera_isp_hardware_release(int fd, int32_t session, int32_t device);

#endif
