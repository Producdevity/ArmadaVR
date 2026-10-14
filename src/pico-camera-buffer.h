#ifndef REFRAMED_PICO_CAMERA_BUFFER_H
#define REFRAMED_PICO_CAMERA_BUFFER_H

#include "ion-buffer.h"
#include "pico-camera-control.h"

struct pico_camera_buffer;
enum pico_camera_buffer_kind {
    PICO_CAMERA_BUFFER_PACKET, PICO_CAMERA_BUFFER_COMMAND, PICO_CAMERA_BUFFER_PIXELS,
};
struct pico_camera_buffer_config {
    enum pico_camera_buffer_kind kind;
    enum pico_camera_memory_mapping mapping;
    size_t size;
    int32_t mmus[16];
    size_t mmu_count;
    uint32_t ion_heap;
    bool cached;
};
struct pico_camera_buffer_view {
    int32_t memory_handle;
    int fd;
    void *data;
    size_t size;
};

/* Owns an allocation, CRM handle and CPU mapping. Descriptors are borrowed and
 * must stay open. A nonnull output must be released even when creation fails:
 * a failed kernel ioctl can still return resources. No device is opened.
 * Size is page-aligned; pixels require IO mapping and an explicit ION heap.
 * Calls are serialized and the object must not be copied. */
int pico_camera_buffer_create(int crm_fd, int ion_fd,
                              const struct pico_camera_buffer_config *config,
                              struct pico_camera_buffer **buffer);
/* A view borrows ownership. Pixel access must be bracketed by CPU begin/end;
 * device use must be bracketed by submit/complete. The caller retains the owner
 * until all command references, capture reservations and borrowed views end. */
bool pico_camera_buffer_view(const struct pico_camera_buffer *buffer,
                             struct pico_camera_buffer_view *view);
int pico_camera_buffer_cpu_begin(struct pico_camera_buffer *buffer, enum ion_buffer_access access);
int pico_camera_buffer_cpu_end(struct pico_camera_buffer *buffer);
/* Mark before any device submission. Even a failed ioctl may have queued DMA. */
int pico_camera_buffer_submit(struct pico_camera_buffer *buffer);
/* Only after the matching completion fence, or independently verified complete
 * DMA quiescence. A flush, timeout or closed event fd is not sufficient. */
int pico_camera_buffer_complete(struct pico_camera_buffer *buffer);
/* Refuses in-flight/CPU-owned buffers. A failed CRM release is never retried:
 * b1977 can free its table entry before returning a cache error. The owner and
 * remaining resources then stay retained for recovery by the enclosing session.
 * After successful unmap, Linux close is attempted once even on EINTR. */
int pico_camera_buffer_release(struct pico_camera_buffer **buffer);

#endif
