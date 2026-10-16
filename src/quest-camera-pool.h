#ifndef REFRAMED_QUEST_CAMERA_POOL_H
#define REFRAMED_QUEST_CAMERA_POOL_H
#include "quest-camera-allocation.h"
#include "quest-camera-capture.h"

struct quest_camera_pool;
struct quest_camera_pool_config {
    int crm_fd, isp_fd, heap_fd;
    int32_t session, device, image_mmu, command_mmu;
    enum quest_camera_memory_mapping command_mapping;
    size_t count, command_size;
    uint32_t width, height, stride, slice_height, bits, rdi, pattern;
};
enum quest_camera_pool_resource {
    QUEST_CAMERA_POOL_PIXELS, QUEST_CAMERA_POOL_COMMAND, QUEST_CAMERA_POOL_PACKET,
    QUEST_CAMERA_POOL_RESOURCES,
};
/* One exclusively acquired ISP, 1..30 fresh cached DMA-heap images and matching
 * streaming templates. Explicit nonsecure MMUs, heap and routing; no discovery
 * or hardware start. Height includes metadata. MIPI RAW8/RAW10 only. Each image
 * gets ID index+1; its page-aligned allocation covers stride*slice_height.
 * Command size is explicit, page-aligned and 4096..1048576 bytes.
 * Retain a nonnull owner after any failure. Inputs are copied; calls are serial. */
int quest_camera_pool_prepare(const struct quest_camera_pool_config *config, struct quest_camera_pool **pool);
/* One SET_STREAM_MODE attempt after all allocations and CPU writes finish.
 * Requires kernel patches 0007 (per-image timing) and 0008 (setup cleanup).
 * Keep the owner after every result until independent quiescence. */
int quest_camera_pool_configure(struct quest_camera_pool *pool);
/* Borrowed capture descriptors after successful configuration, before hardware
 * start. Pass the pool's token unchanged to cpu_sync, only after a successful GET
 * for that same image. These views remain valid until final quiescence. */
bool quest_camera_pool_capture_buffers(struct quest_camera_pool *pool,
    struct quest_camera_capture_buffer *buffers, size_t count);
/* Capture callback: GET must establish exclusive ownership before begin; RETURN
 * must follow successful end. At most one image may be held. Any cache failure
 * poisons the pool; no automatic retry, return or resource disposal follows. */
bool quest_camera_pool_cpu_sync(void *pool, void *token, bool end);
/* Independent device/DMA/CPU/control quiescence, including partial preparation.
 * Does not stop hardware or repair an ambiguous ioctl. */
int quest_camera_pool_quiesced(struct quest_camera_pool *pool);
/* One cleanup attempt. Ambiguous duplicate resources require external cleanup. */
int quest_camera_pool_release(struct quest_camera_pool **pool);
bool quest_camera_pool_resources(const struct quest_camera_pool *pool, size_t image,
    enum quest_camera_pool_resource resource, struct quest_camera_allocation_view *view);
/* Local disposal after independent quiescence and external cleanup of every
 * owned mapping, fd and CRM reference, including unpublished allocation fds. */
void quest_camera_pool_closed(struct quest_camera_pool **pool);
#endif
