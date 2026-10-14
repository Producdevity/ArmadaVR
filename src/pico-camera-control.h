#ifndef REFRAMED_PICO_CAMERA_CONTROL_H
#define REFRAMED_PICO_CAMERA_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* b1977 Linux ioctl transport. Descriptors must already refer to the matching
 * CRM/sync/device node. No discovery, open, retry, stream startup or cleanup is
 * automatic. Returns zero or negative errno. Handle outputs start at zero.
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

#endif
