#ifndef REFRAMED_PICO_CAMERA_CONTROL_H
#define REFRAMED_PICO_CAMERA_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* b1977 Linux ioctl transport. Descriptors must already refer to the matching
 * CRM/sync/device node. No discovery, open, retry, stream startup or cleanup is
 * automatic. Returns zero or negative errno. Handle outputs start at zero;
 * positive returned handles are retained even after a failed copy-back.
 * Failed mutating ioctls may have partly completed: stop the owner and establish
 * quiescence before reusing resources. A successful CRM flush alone does not
 * establish hardware quiescence. */
int pico_camera_session_create(int crm_fd, int32_t *session);
int pico_camera_session_destroy(int crm_fd, int32_t *session);
int pico_camera_link_create(int crm_fd, int32_t session, const int32_t *devices,
                            size_t count, int32_t *link);
int pico_camera_link_destroy(int crm_fd, int32_t session, int32_t *link);
int pico_camera_link_activate(int crm_fd, int32_t session, int32_t link, bool active);
int pico_camera_links_synchronize(int crm_fd, int32_t session, const int32_t links[2], int32_t master);
int pico_camera_request_schedule(int crm_fd, int32_t session, int32_t link,
                                 uint64_t request_id, bool synchronized, bool bubble_recovery);
int pico_camera_requests_flush(int crm_fd, int32_t session, int32_t link);
int pico_camera_device_configure(int device_fd, int32_t session, int32_t device,
                                 int32_t packet, uint64_t offset, uint64_t allocation_size);
int pico_camera_device_stream(int device_fd, int32_t session, int32_t device, bool start);
int pico_camera_device_release(int device_fd, int32_t session, int32_t *device);
int pico_camera_fence_create(int sync_fd, const char *name, int32_t *fence);
int pico_camera_fence_register(int sync_fd, int32_t fence, uint64_t generation, uint64_t request_id);
int pico_camera_fence_destroy(int sync_fd, int32_t *fence);

struct pico_camera_sensor_capabilities {
    uint32_t slot, secure, pitch, roll, yaw;
    uint32_t actuator_slot, eeprom_slot, ois_slot, flash_slot, phy_slot;
};
struct pico_camera_phy_capabilities { uint32_t slot, version, clock_lane; };
struct pico_camera_ife_capabilities {
    int32_t device_mmu, cdm_mmu;
    uint32_t device_count;
    struct { uint32_t type, major, minor, increment; } devices[5];
};
struct pico_camera_ife_hardware {
    uint32_t id[5], paths[5][3], valid;
};

/* Queries preserve output on failure. MMU handles come from the device query;
 * absent MMUs remain negative. These calls do not acquire or start devices. */
int pico_camera_sensor_query(int sensor_fd, struct pico_camera_sensor_capabilities *caps);
int pico_camera_phy_query(int phy_fd, struct pico_camera_phy_capabilities *caps);
int pico_camera_ife_query(int ife_fd, struct pico_camera_ife_capabilities *caps);

/* Probe uses a packet at offset zero with kernel-mapped command buffers.
 * Probing powers the sensor; this is not a read-only device operation. */
int pico_camera_sensor_probe_submit(int sensor_fd, int32_t packet);

/* Acquisition outputs start at zero. A returned positive handle is preserved
 * even on ioctl failure: the sensor kernel copies it before powering up.
 * Such failure requires shutdown before reuse, not an acquisition retry.
 * PHY handles must not be included in CRM links. IFE device acquisition uses
 * the split API and must be followed by hardware acquisition before config. */
int pico_camera_sensor_acquire(int sensor_fd, int32_t session, int32_t *device);
int pico_camera_phy_acquire(int phy_fd, int32_t session, bool combo, int32_t *device);
int pico_camera_ife_device_acquire(int ife_fd, int32_t session, int32_t *device);
int pico_camera_ife_hardware_acquire(int ife_fd, int32_t session, int32_t device,
                                    const void *resource, size_t size,
                                    struct pico_camera_ife_hardware *hardware);
int pico_camera_ife_hardware_release(int ife_fd, int32_t session, int32_t device);

enum pico_camera_memory_kind { PICO_CAMERA_MEMORY_PACKET, PICO_CAMERA_MEMORY_COMMAND };
enum pico_camera_memory_mapping {
    PICO_CAMERA_MEMORY_CPU, PICO_CAMERA_MEMORY_IO, PICO_CAMERA_MEMORY_SHARED,
};
struct pico_camera_memory_allocation { int32_t handle; int fd; };

/* Allocate packet/command memory through CRM so it has a kernel mapping.
 * Output starts at {0, -1}; returned resources are preserved even on errors.
 * Size is a nonzero multiple of 4096, at most INT32_MAX. MMU handles must come
 * from a capability query. Packets use CPU mapping and no MMUs; commands may
 * use explicit IO/shared mapping. CPU access/cache synchronization is separate.
 * The caller owns the returned fd and must close it after releasing its CRM
 * handle and userspace mapping. This function does not map or close fds. */
int pico_camera_memory_allocate(int crm_fd, enum pico_camera_memory_kind kind,
                                enum pico_camera_memory_mapping mapping, uint64_t size,
                                const int32_t *mmus, size_t count, bool cached,
                                struct pico_camera_memory_allocation *allocation);

/* Import a caller-owned pixel DMA-BUF into explicit nonsecure device MMUs.
 * Imports have no kernel mapping and cannot carry commands or packets.
 * The fd stays owned by the caller. Handle output starts at zero. */
int pico_camera_pixels_import(int crm_fd, int dma_fd, const int32_t *mmus,
                              size_t count, bool cached, int32_t *handle);

/* Release only after independent DMA quiescence. This neither closes the
 * DMA-BUF fd nor unmaps userspace memory; failures preserve the handle. */
int pico_camera_memory_release(int crm_fd, int32_t *handle);

struct pico_camera_event {
    uint32_t type, id, sequence, pending;
    uint8_t data[64];
};

/* V4L2 event transport for exclusively owned, nonblocking sync/CRM descriptors.
 * Subscribe ID 0 on sync; IDs 0 (SOF) and 1 (error) on CRM. No retries or loops.
 * Dequeue returns zero or negative errno, including -EAGAIN when empty, and
 * preserves output on failure. Sequence is retained to detect lost events;
 * the V4L2 wrapper timestamp is not a calibrated camera timestamp. */
int pico_camera_event_subscribe(int event_fd, uint32_t id, bool enabled);
int pico_camera_event_dequeue(int event_fd, struct pico_camera_event *event);

#endif
