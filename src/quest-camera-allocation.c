#include "quest-camera-allocation.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct quest_camera_allocation {
    int crm_fd;
    struct quest_camera_allocation_view view;
    enum quest_camera_cpu_access cpu;
    bool ready, poisoned, uncertain, submitted, in_flight, quiesced, releasing;
    int release_error;
};
static bool valid(const struct quest_camera_allocation_config *c)
{
    if (!c || c->kind < QUEST_CAMERA_ALLOCATION_PACKET || c->kind > QUEST_CAMERA_ALLOCATION_PIXELS ||
        c->mapping < QUEST_CAMERA_MEMORY_CPU || c->mapping > QUEST_CAMERA_MEMORY_SHARED ||
        !c->size || c->size > INT32_MAX || c->size % 4096 || c->mmu_count > 16 ||
        ((c->mapping == QUEST_CAMERA_MEMORY_CPU) != (c->mmu_count == 0)) ||
        (c->kind == QUEST_CAMERA_ALLOCATION_PACKET && c->mapping != QUEST_CAMERA_MEMORY_CPU) ||
        (c->kind == QUEST_CAMERA_ALLOCATION_PIXELS && c->mapping != QUEST_CAMERA_MEMORY_IO)) return false;
    for (size_t i = 0; i < c->mmu_count; ++i) {
        if (c->mmus[i] < 0) return false;
        for (size_t j = 0; j < i; ++j) if (c->mmus[i] == c->mmus[j]) return false;
    }
    return true;
}
static bool valid_handle(int32_t handle, int fd)
{
    return handle > 0 && fd >= 0 && fd <= 32767 && ((uint32_t)handle >> 16) == (uint32_t)fd &&
        (handle & 0x7fff) && !(handle & 0x8000);
}
static int pixels(struct quest_camera_allocation *b, int heap, const struct quest_camera_allocation_config *c)
{
    struct heap_allocation { uint64_t length; uint32_t fd, fd_flags; uint64_t heap_flags; } request = {c->size, 0, 0x80002, 0};
    _Static_assert(sizeof(request) == 24 && offsetof(struct heap_allocation, fd) == 8, "Quest DMA heap ABI");
    int result = ioctl(heap, 0xc0184800UL, &request);
    int error = result < 0 ? -errno : result ? -EPROTO : 0;
    if (request.fd <= INT_MAX && (!error || request.fd) && (int)request.fd != heap && (int)request.fd != b->crm_fd)
        b->view.fd = (int)request.fd;
    if (error || b->view.fd < 0) { b->uncertain = b->view.fd < 0; return error ? error : -EPROTO; }
    if (b->view.fd > 32767) return -ERANGE;
    result = quest_camera_pixels_import(b->crm_fd, b->view.fd, c->mmus, c->mmu_count,
                                        (uint32_t)c->size, &b->view.handle);
    if (!valid_handle(b->view.handle, b->view.fd)) b->uncertain = true;
    return result;
}
int quest_camera_allocation_create(int crm_fd, int heap_fd,
    const struct quest_camera_allocation_config *c, struct quest_camera_allocation **allocation)
{
    if (!allocation || *allocation || crm_fd < 0 || !valid(c) ||
        (c->kind == QUEST_CAMERA_ALLOCATION_PIXELS && (heap_fd < 0 || heap_fd == crm_fd))) return -EINVAL;
    struct quest_camera_allocation *b = calloc(1, sizeof(*b));
    if (!b) return -ENOMEM;
    *allocation = b; b->crm_fd = crm_fd; b->view.fd = -1; b->view.size = c->size;
    int result;
    if (c->kind == QUEST_CAMERA_ALLOCATION_PIXELS) result = pixels(b, heap_fd, c);
    else {
        struct quest_camera_memory_allocation memory = {.fd = -1};
        result = quest_camera_memory_allocate(crm_fd,
            c->kind == QUEST_CAMERA_ALLOCATION_PACKET ? QUEST_CAMERA_MEMORY_PACKET : QUEST_CAMERA_MEMORY_COMMAND,
            c->mapping, c->size, c->mmus, c->mmu_count, &memory);
        b->view.handle = memory.handle;
        if (memory.fd != crm_fd) b->view.fd = memory.fd;
        b->uncertain = !valid_handle(b->view.handle, b->view.fd);
    }
    if (!result) {
        int flags = fcntl(b->view.fd, F_GETFD);
        result = flags < 0 ? -errno : !(flags & FD_CLOEXEC) ? -EPROTO : 0;
    }
    if (!result) {
        void *data = mmap(NULL, c->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->view.fd, 0);
        if (data == MAP_FAILED) result = -errno;
        else { b->view.data = data; b->view.mapped = true; if (!data) result = -EPROTO; }
    }
    b->ready = result == 0; b->poisoned = result != 0;
    return result;
}
bool quest_camera_allocation_resources(const struct quest_camera_allocation *b, struct quest_camera_allocation_view *view)
{
    if (!b || !view) return false;
    *view = b->view;
    return true;
}
bool quest_camera_allocation_view(const struct quest_camera_allocation *b, struct quest_camera_allocation_view *view)
{
    return b && b->ready && !b->poisoned && !b->quiesced && !b->releasing && quest_camera_allocation_resources(b, view);
}
static int sync_buffer(struct quest_camera_allocation *b, enum quest_camera_cpu_access access, bool end)
{
    uint64_t flags = (uint64_t)access | (end ? 4u : 0u);
    int result = ioctl(b->view.fd, 0x40086200UL, &flags);
    return result < 0 ? -errno : result ? -EPROTO : 0;
}
int quest_camera_allocation_cpu_begin(struct quest_camera_allocation *b, enum quest_camera_cpu_access access)
{
    if (!b || !b->ready || b->poisoned || b->quiesced || b->releasing ||
        access < QUEST_CAMERA_CPU_READ || access > QUEST_CAMERA_CPU_READ_WRITE) return -EINVAL;
    if (b->in_flight || b->cpu) return -EBUSY;
    int result = sync_buffer(b, access, false);
    if (result) b->poisoned = true;
    else b->cpu = access;
    return result;
}
int quest_camera_allocation_cpu_end(struct quest_camera_allocation *b)
{
    if (!b || !b->cpu || b->quiesced || b->releasing) return -EINVAL;
    int result = sync_buffer(b, b->cpu, true);
    b->cpu = 0;
    if (result) b->poisoned = true;
    return result;
}
int quest_camera_allocation_submit(struct quest_camera_allocation *b)
{
    if (!b || !b->ready || b->poisoned || b->quiesced || b->releasing) return -EINVAL;
    if (b->in_flight || b->cpu) return -EBUSY;
    b->submitted = b->in_flight = true;
    return 0;
}
int quest_camera_allocation_complete(struct quest_camera_allocation *b)
{
    if (!b || !b->in_flight || b->quiesced || b->releasing) return -EINVAL;
    b->in_flight = false;
    return 0;
}
int quest_camera_allocation_quiesced(struct quest_camera_allocation *b)
{
    if (!b || b->quiesced || b->releasing) return -EINVAL;
    b->quiesced = true; b->in_flight = false; b->cpu = 0;
    return 0;
}
int quest_camera_allocation_release(struct quest_camera_allocation **allocation)
{
    if (!allocation) return -EINVAL;
    struct quest_camera_allocation *b = *allocation;
    if (!b) return 0;
    if (!b->quiesced && (b->in_flight || b->cpu || b->poisoned || b->submitted)) return -EBUSY;
    if (b->releasing) return b->release_error;
    b->releasing = true; b->ready = false;
    if (b->uncertain) return b->release_error = -EIO;
    if (b->view.handle) {
        int result = quest_camera_memory_release(b->crm_fd, &b->view.handle);
        if (result) return b->release_error = result;
    }
    if (b->view.mapped) {
        if (munmap(b->view.data, b->view.size)) return b->release_error = -errno;
        b->view.data = NULL; b->view.mapped = false;
    }
    int result = 0;
    if (b->view.fd >= 0 && close(b->view.fd)) result = -errno;
    free(b); *allocation = NULL;
    return result;
}
void quest_camera_allocation_closed(struct quest_camera_allocation **allocation)
{
    if (allocation) { free(*allocation); *allocation = NULL; }
}
