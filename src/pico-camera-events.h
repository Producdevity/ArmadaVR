#ifndef REFRAMED_PICO_CAMERA_EVENTS_H
#define REFRAMED_PICO_CAMERA_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_CAMERA_EVENT_DATA_SIZE 64
#define PICO_CAMERA_FENCE_PAYLOAD_SIZE 24

enum pico_camera_event_result {
    PICO_CAMERA_EVENT_UNRELATED,
    PICO_CAMERA_EVENT_VALID,
    PICO_CAMERA_EVENT_MALFORMED,
};

struct pico_camera_fence_event {
    int32_t fence;
    uint32_t status;
    uint64_t generation, request_id;
};
struct pico_camera_sof_event {
    int32_t session, link;
    uint64_t request_id, frame_number;
    int64_t qtimer_ns;
    uint32_t status;
};
struct pico_camera_error_event {
    int32_t session, device, link;
    uint32_t type, request_id;
    uint64_t resource_size;
};

/* Decode the 64-byte V4L2 event data from the corresponding device fd, not the
 * full platform-dependent v4l2_event structure. Reserved bytes may be uninitialized
 * in b1977; only defined fields are read. Failure leaves output unchanged. */
enum pico_camera_event_result pico_camera_fence_event_decode(
    uint32_t type, uint32_t id, const void *data, size_t size, struct pico_camera_fence_event *event);
enum pico_camera_event_result pico_camera_sof_event_decode(
    uint32_t type, uint32_t id, const void *data, size_t size, struct pico_camera_sof_event *event);
enum pico_camera_event_result pico_camera_error_event_decode(
    uint32_t type, uint32_t id, const void *data, size_t size, struct pico_camera_error_event *event);

/* SOF_BOOT_TS is unrelated to the SOF decoder: boottime is not qtimer or monotonic.
 * SOF request zero is valid but has no scheduled buffer. An error event carries
 * only 32 request bits; invalidate its matching session/link instead of treating
 * those bits as an unambiguous 64-bit request identity. */

/* Register the generation/request cookie before submitting the fenced request.
 * This encodes cam_sync_userpayload_info only; no ioctl is issued. */
bool pico_camera_fence_payload(int32_t fence, uint64_t generation, uint64_t request_id,
                                void *output, size_t size);

#endif
