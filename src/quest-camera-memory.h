#ifndef REFRAMED_QUEST_CAMERA_MEMORY_H
#define REFRAMED_QUEST_CAMERA_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum quest_camera_memory_kind { QUEST_CAMERA_MEMORY_PACKET, QUEST_CAMERA_MEMORY_COMMAND };
enum quest_camera_memory_mapping {
    QUEST_CAMERA_MEMORY_CPU, QUEST_CAMERA_MEMORY_IO, QUEST_CAMERA_MEMORY_SHARED,
};
struct quest_camera_memory_allocation { int32_t handle; int fd; };

/* Explicit exclusive CRM descriptor, cached nonsecure memory only. Quest uses
 * DMA heaps, not Pico's ION allocator. Packets require CPU-only mapping; commands
 * may use query-derived IO/shared MMUs. Size is page-aligned and <= INT32_MAX.
 * Output starts at {0, -1}; returned resource values survive failure, including unsupported handle bits. The caller
 * owns the DMA-BUF fd and its later mapping/cache access. No implicit retries. */
int quest_camera_memory_allocate(int fd, enum quest_camera_memory_kind kind,
                                 enum quest_camera_memory_mapping mapping, uint64_t size,
                                 const int32_t *mmus, size_t count,
                                 struct quest_camera_memory_allocation *allocation);
/* Import a fresh caller-owned cached DMA-BUF, never an already imported or CRM
 * allocated buffer. Its verified page-aligned allocation size must match the
 * mapped extent. Output starts at zero; nonzero handle bits survive all errors.
 * The caller retains the fd, mapping and CPU-cache synchronization ownership. */
int quest_camera_pixels_import(int fd, int dma_fd, const int32_t *mmus,
                               size_t count, uint32_t allocation_size, int32_t *handle);
/* Only after independently verified DMA quiescence. Clears the handle only on
 * success; failures may already have unmapped/freed it and must not be retried.
 * This call neither closes the fd nor unmaps userspace memory. */
int quest_camera_memory_release(int fd, int32_t *handle);
#endif
