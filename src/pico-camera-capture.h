#ifndef REFRAMED_PICO_CAMERA_CAPTURE_H
#define REFRAMED_PICO_CAMERA_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_CAMERA_CAPTURE_MAX_PENDING 16

struct pico_camera_capture;
struct pico_camera_capture_config {
    uint64_t generation;
    uint8_t camera_ids[4];
    size_t camera_count;
    int64_t timeout_ns;
};

struct pico_camera_capture_buffer {
    void *token;
    uint64_t request_id;
    int32_t memory_handle, fence;
    uint8_t camera_id;
    uint32_t width, height, stride;
    const uint8_t *pixels;
    size_t size;
};

struct pico_camera_capture_frame {
    struct pico_camera_capture_buffer buffer;
    uint64_t frame_number;
    int64_t center_ns;
};

struct pico_camera_capture_ops {
    bool (*cpu_sync)(void *opaque, void *token, bool end);
    bool (*frame)(void *opaque, uint64_t generation,
                  const struct pico_camera_capture_frame *frame, int64_t now_ns);
    bool (*release)(void *opaque, void *token);
    void (*invalidate)(void *opaque, uint64_t generation);
};

enum pico_camera_capture_result {
    PICO_CAMERA_CAPTURE_IGNORED,
    PICO_CAMERA_CAPTURE_PENDING,
    PICO_CAMERA_CAPTURE_DELIVERED,
    PICO_CAMERA_CAPTURE_FAILED,
};

/* Calls and callbacks must be serialized and non-reentrant. No devices are
 * opened. The transport owns allocations; a successful reserve borrows them
 * until release succeeds. release must support retry after a partial failure.
 * frame must copy the visible pixels before returning. invalidate ends tracking
 * immediately; it does not establish DMA quiescence. */
bool pico_camera_capture_create(const struct pico_camera_capture_config *config,
                                 const struct pico_camera_capture_ops *ops, void *opaque,
                                 struct pico_camera_capture **capture);

/* Reserve before any request is submitted to a device. IDs strictly increase
 * per camera within the generation and must fit the kernel's signed 64 bits.
 * On a subsequent submission error, call stop; do not recycle the reservation.
 * A false return transfers no ownership. Call poll regularly even if silent. */
bool pico_camera_capture_reserve(struct pico_camera_capture *capture,
                                  const struct pico_camera_capture_buffer *buffer, int64_t now_ns);
bool pico_camera_capture_poll(struct pico_camera_capture *capture, int64_t now_ns);

/* Match the fence event's registered generation/request cookie as well as its
 * fence handle. Success/error states are the b1977 sync ABI values 2/3. Old or
 * unrelated events are ignored. Timing must be a verified exposure-center time
 * in the configured camera clock; neither raw SOF nor boot-time is sufficient. */
enum pico_camera_capture_result pico_camera_capture_fence(
    struct pico_camera_capture *capture, uint64_t generation, uint64_t request_id,
    int32_t fence, uint32_t status, int64_t now_ns);
enum pico_camera_capture_result pico_camera_capture_timing(
    struct pico_camera_capture *capture, uint64_t generation, uint64_t request_id,
    uint8_t camera_id, uint64_t frame_number, int64_t center_ns, int64_t now_ns);

void pico_camera_capture_stop(struct pico_camera_capture *capture);
/* Only after the transport independently confirms every device has stopped DMA.
 * A timeout, failed ioctl or closed event fd is not that confirmation. */
bool pico_camera_capture_quiesced(struct pico_camera_capture *capture);
/* Refuses to destroy an owner with unreleased reservations. */
bool pico_camera_capture_destroy(struct pico_camera_capture **capture);

#endif
