#include "pico-camera-buffer.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

struct pico_camera_buffer {
    int crm_fd;
    struct ion_buffer memory;
    struct pico_camera_memory_allocation allocation;
    enum ion_buffer_access cpu_access;
    bool ready, in_flight, poisoned, release_failed;
};

static bool valid(const struct pico_camera_buffer_config *c)
{
    if (!c || c->kind < PICO_CAMERA_BUFFER_PACKET || c->kind > PICO_CAMERA_BUFFER_PIXELS ||
        c->mapping < PICO_CAMERA_MEMORY_CPU || c->mapping > PICO_CAMERA_MEMORY_SHARED ||
        !c->size || c->size > INT32_MAX || c->size % 4096 || c->mmu_count > 16) return false;
    if ((c->mapping == PICO_CAMERA_MEMORY_CPU) != (c->mmu_count == 0) ||
        (c->kind == PICO_CAMERA_BUFFER_PACKET && c->mapping != PICO_CAMERA_MEMORY_CPU) ||
        (c->kind == PICO_CAMERA_BUFFER_PIXELS && (c->mapping != PICO_CAMERA_MEMORY_IO || c->ion_heap >= 32))) return false;
    for (size_t i = 0; i < c->mmu_count; ++i) {
        if (c->mmus[i] < 0) return false;
        for (size_t j = 0; j < i; ++j) if (c->mmus[i] == c->mmus[j]) return false;
    }
    return true;
}

int pico_camera_buffer_create(int crm_fd, int ion_fd,
                              const struct pico_camera_buffer_config *config,
                              struct pico_camera_buffer **buffer)
{
    if (!buffer || *buffer || crm_fd < 0 || !valid(config) ||
        (config->kind == PICO_CAMERA_BUFFER_PIXELS && ion_fd < 0)) return -EINVAL;
    struct pico_camera_buffer *b = calloc(1, sizeof(*b));
    if (!b) return -ENOMEM;
    b->crm_fd = crm_fd; b->memory.fd = -1; b->allocation.fd = -1;
    *buffer = b;
    int result;
    if (config->kind == PICO_CAMERA_BUFFER_PIXELS) {
        result = ion_buffer_allocate(ion_fd, config->ion_heap, config->size, config->cached, &b->memory);
        if (!result) result = pico_camera_pixels_import(crm_fd, b->memory.fd, config->mmus,
            config->mmu_count, config->cached, &b->allocation.handle);
    } else {
        result = pico_camera_memory_allocate(crm_fd,
            config->kind == PICO_CAMERA_BUFFER_PACKET ? PICO_CAMERA_MEMORY_PACKET : PICO_CAMERA_MEMORY_COMMAND,
            config->mapping, config->size, config->mmus, config->mmu_count, config->cached, &b->allocation);
        if (!result) {
            result = ion_buffer_map(b->allocation.fd, config->size, &b->memory);
            if (!result) b->allocation.fd = -1;
        }
    }
    b->ready = result == 0;
    b->poisoned = result != 0;
    return result;
}

bool pico_camera_buffer_view(const struct pico_camera_buffer *b, struct pico_camera_buffer_view *view)
{
    if (!b || !view || !b->ready || b->poisoned) return false;
    *view = (struct pico_camera_buffer_view){b->allocation.handle, b->memory.fd, b->memory.data, b->memory.size};
    return true;
}
int pico_camera_buffer_cpu_begin(struct pico_camera_buffer *b, enum ion_buffer_access access)
{
    if (!b || !b->ready || b->poisoned || access < ION_BUFFER_READ || access > ION_BUFFER_READ_WRITE) return -EINVAL;
    if (b->in_flight || b->cpu_access) return -EBUSY;
    int result = ion_buffer_sync(&b->memory, access, false);
    if (result) b->poisoned = true;
    else b->cpu_access = access;
    return result;
}
int pico_camera_buffer_cpu_end(struct pico_camera_buffer *b)
{
    if (!b || !b->cpu_access) return -EINVAL;
    int result = ion_buffer_sync(&b->memory, b->cpu_access, true);
    b->cpu_access = 0;
    if (result) b->poisoned = true;
    return result;
}
int pico_camera_buffer_submit(struct pico_camera_buffer *b)
{
    if (!b || !b->ready || b->poisoned) return -EINVAL;
    if (b->in_flight || b->cpu_access) return -EBUSY;
    b->in_flight = true;
    return 0;
}
int pico_camera_buffer_complete(struct pico_camera_buffer *b)
{
    if (!b || !b->in_flight) return -EINVAL;
    b->in_flight = false;
    return 0;
}
int pico_camera_buffer_release(struct pico_camera_buffer **buffer)
{
    if (!buffer) return -EINVAL;
    struct pico_camera_buffer *b = *buffer;
    if (!b) return 0;
    if (b->in_flight || b->cpu_access) return -EBUSY;
    if (b->release_failed) return -EIO;
    b->ready = false;
    if (b->allocation.handle > 0) {
        int result = pico_camera_memory_release(b->crm_fd, &b->allocation.handle);
        if (result) { b->release_failed = true; return result; }
    }
    int result = 0;
    if (b->memory.fd >= 0) {
        result = ion_buffer_release(&b->memory);
        if (b->memory.fd >= 0) return result;
    }
    if (b->allocation.fd >= 0 && close(b->allocation.fd)) result = -errno;
    free(b); *buffer = NULL;
    return result;
}
