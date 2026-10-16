#ifndef REFRAMED_QUEST_CAMERA_BUFFER_H
#define REFRAMED_QUEST_CAMERA_BUFFER_H
#include <stddef.h>
#include <stdint.h>

struct quest_camera_buffer {
    uint64_t id;
    int32_t pixels, packet;
    uint64_t packet_offset, packet_allocation_size;
};
struct quest_camera_frame {
    uint64_t id;
    int64_t qtimer_ns, observed_monotonic_ns, frame_number;
};

/* Exclusive acquired ISP descriptor and already prepared single-plane packets.
 * Setup is one attempt; buffers, memory handles and packets remain owned by the
 * caller until independent DMA quiescence. None of these calls opens a node,
 * starts capture, maps memory, retries or synchronizes the DMA-BUF CPU cache.
 * Any failed mutation can have changed kernel state, including failed GET
 * copy-back after a buffer moved to the userspace-owned list. Stop on errors
 * other than GET reporting no ready image (-ETIMEDOUT or -ENOENT). */
int quest_camera_buffers_configure(int fd, int32_t session, int32_t device,
                                   const struct quest_camera_buffer *buffers, size_t count);
/* GET uses zero wait timeout and accepts exactly one returned frame. Match its
 * ID to an owned buffer before accessing pixels; output is unchanged on error.
 * qtimer_ns is the buffer's captured SOF, not exposure center. The vendor fills
 * observed_monotonic_ns from the current context at dequeue in the stock kernel.
 * Patch 0007 saves that observation with each queued image. Require that exact
 * kernel fix before using the pair for alignment; neither value is exposure
 * center, and the timestamp sampling uncertainty must still be established. */
int quest_camera_buffer_get(int fd, int32_t session, int32_t device, struct quest_camera_frame *frame);
/* Return one held buffer only after the consumer and CPU cache access finish.
 * GET and RETURN are separate calls so partial compound completion is avoided. */
int quest_camera_buffer_return(int fd, int32_t session, int32_t device, uint64_t id);
#endif
