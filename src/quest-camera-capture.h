#ifndef REFRAMED_QUEST_CAMERA_CAPTURE_H
#define REFRAMED_QUEST_CAMERA_CAPTURE_H
#include "quest-camera-buffer.h"
#include "quest-camera-pixels.h"
#include "quest-camera-shutter.h"

struct quest_camera_capture;
struct quest_camera_capture_buffer {
    uint64_t id;
    void *token;
    const void *pixels;
    size_t size;
};
struct quest_camera_capture_config {
    int fd;
    int32_t session, device;
    uint64_t generation;
    uint32_t camera_bit, metadata_bits;
    enum quest_camera_sensor sensor;
    struct quest_camera_metadata_parameters metadata;
    struct quest_camera_pixel_layout layout;
    size_t metadata_size, buffer_count;
    struct quest_camera_capture_buffer buffers[30];
    int64_t timeout_ns;
    bool require_auxiliary_tag;
};
struct quest_camera_capture_frame {
    const uint8_t *pixels;
    uint32_t width, height;
    int64_t center_ns;
    struct quest_camera_frame image;
    struct quest_camera_metadata metadata;
    struct quest_camera_shutter shutter;
};
struct quest_camera_capture_ops {
    bool (*cpu_sync)(void *opaque, void *token, bool end);
    bool (*window)(void *opaque, const struct quest_camera_frame *image,
                   const struct quest_camera_metadata *metadata, int64_t now_ns,
                   uint64_t *minimum_us, uint64_t *maximum_us);
    bool (*center)(void *opaque, const struct quest_camera_frame *image,
                   const struct quest_camera_metadata *metadata,
                   const struct quest_camera_shutter *shutter, int64_t now_ns, int64_t *center_ns);
    bool (*frame)(void *opaque, uint64_t generation,
                  const struct quest_camera_capture_frame *frame, int64_t now_ns);
    void (*invalidate)(void *opaque, uint64_t generation);
};

/* One exclusively owned, configured ISP stream. All calls/callbacks are serial
 * and non-reentrant. No fd is opened, hardware started or memory mapped here.
 * Borrowed buffers must match the configured ISP IDs and remain alive through
 * quiescence. They contain one embedded metadata row followed by image rows.
 * The time callbacks must use independently verified, still-valid clock and
 * exposure relationships. window returns raw MCU timestamp bounds; center uses
 * the camera domain selected for tracking. No default relationship is supplied.
 * frame must copy the mono8 pixels before returning. A callback failure stops
 * this generation; invalidate must also stop its tracking consumer. */
bool quest_camera_capture_create(const struct quest_camera_capture_config *config,
                                  const struct quest_camera_capture_ops *ops, void *opaque,
                                  struct quest_camera_capture **capture);
/* Call before starting hardware. One attempt per owner; starts the silence clock. */
bool quest_camera_capture_start(struct quest_camera_capture *capture, int64_t now_ns);
/* Already decoded shutter for the selected camera. Foreign generations are
 * ignored. At most 16 pending records; overflow and duplicates fail closed. */
bool quest_camera_capture_shutter(struct quest_camera_capture *capture, uint64_t generation,
                                   const struct quest_camera_shutter *shutter, int64_t now_ns);
/* At most one nonblocking GET and RETURN per call. 1 delivered, 0 pending, negative
 * errno on failure. Poll even during silence. An unknown ID, ambiguous GET/RETURN
 * failure, stale frame, clock regression or failed CPU access stops the owner;
 * no buffer is then returned or reused automatically. */
int quest_camera_capture_poll(struct quest_camera_capture *capture, int64_t now_ns);
void quest_camera_capture_stop(struct quest_camera_capture *capture);
/* The caller must independently establish capture DMA and any uncertain CPU
 * cache access have ended. This only releases this owner's logical borrow;
 * it does not issue cleanup ioctls, retry a failed RETURN or free allocations. */
bool quest_camera_capture_quiesced(struct quest_camera_capture *capture);
bool quest_camera_capture_destroy(struct quest_camera_capture **capture);
#endif
