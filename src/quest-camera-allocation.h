#ifndef REFRAMED_QUEST_CAMERA_ALLOCATION_H
#define REFRAMED_QUEST_CAMERA_ALLOCATION_H
#include "quest-camera-memory.h"

enum quest_camera_allocation_kind {
    QUEST_CAMERA_ALLOCATION_PACKET, QUEST_CAMERA_ALLOCATION_COMMAND, QUEST_CAMERA_ALLOCATION_PIXELS,
};
enum quest_camera_cpu_access { QUEST_CAMERA_CPU_READ = 1, QUEST_CAMERA_CPU_WRITE = 2, QUEST_CAMERA_CPU_READ_WRITE = 3 };
struct quest_camera_allocation_config {
    enum quest_camera_allocation_kind kind;
    enum quest_camera_memory_mapping mapping;
    size_t size, mmu_count;
    int32_t mmus[16];
};
struct quest_camera_allocation_view { int32_t handle; int fd; void *data; size_t size; bool mapped; };
struct quest_camera_allocation;

/* Exclusively borrowed CRM and, for pixels, an identified cached DMA-heap fd.
 * No node or heap is selected/opened here. Packets/commands use CRM allocation;
 * pixels use a fresh DMA-heap allocation followed by CRM import. Inputs must be
 * page aligned and reference verified MMUs. The owner retains every returned
 * resource, including partial results after failure. A nonnull output must be
 * retained until release or independently completed cleanup. Calls are serial. */
int quest_camera_allocation_create(int crm_fd, int heap_fd,
    const struct quest_camera_allocation_config *config, struct quest_camera_allocation **allocation);
/* View only a usable allocation. Bracket CPU access even for initial clearing. */
bool quest_camera_allocation_view(const struct quest_camera_allocation *allocation,
                                  struct quest_camera_allocation_view *view);
int quest_camera_allocation_cpu_begin(struct quest_camera_allocation *allocation, enum quest_camera_cpu_access access);
int quest_camera_allocation_cpu_end(struct quest_camera_allocation *allocation);
int quest_camera_allocation_submit(struct quest_camera_allocation *allocation);
/* Caller must have independently verified completion of every device use of
 * this allocation. A control ioctl return or timeout alone is insufficient. */
int quest_camera_allocation_complete(struct quest_camera_allocation *allocation);
/* Final independent DMA/CPU/control quiescence; no further use is allowed.
 * Required after submission or uncertain allocation/cache operations. */
int quest_camera_allocation_quiesced(struct quest_camera_allocation *allocation);
/* One cleanup attempt, without retries. Unidentified kernel resources remain
 * uncertain and prevent automatic release. Linux close is attempted at most
 * once even on EINTR; a failed munmap retains the mapping for explicit cleanup. */
int quest_camera_allocation_release(struct quest_camera_allocation **allocation);
/* Cleanup inspection only, including poisoned owners; not permission for CPU
 * or device access. Records all still-owned, known resources. */
bool quest_camera_allocation_resources(const struct quest_camera_allocation *allocation,
                                       struct quest_camera_allocation_view *view);
/* Local disposal after independent quiescence and external cleanup of every
 * mapping, owned fd and CRM resource, including any unpublished allocation fd.
 * A DMA-heap copy-back failure may require exiting the isolated owning process;
 * closing the heap or CRM fd alone cannot recover an unidentified DMA-BUF. */
void quest_camera_allocation_closed(struct quest_camera_allocation **allocation);
#endif
