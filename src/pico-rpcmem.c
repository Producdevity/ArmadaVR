#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rpcmem.h"
#include "ion-buffer.h"

int pico_rpcmem_register(void *buffer, size_t size, int fd);

struct allocation {
    struct ion_buffer buffer;
    size_t requested_size;
    int registered;
    struct allocation *next;
};
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static struct allocation *allocations;
static int ion_fd = -1;
static int default_heap = -1;

void rpcmem_init(void)
{
    pthread_mutex_lock(&mutex);
    if (ion_fd < 0) {
        const char *value = getenv("PICO_ION_HEAP_ID");
        default_heap = -1;
        if (value && *value >= '0' && *value <= '9') {
            char *end;
            errno = 0;
            unsigned long heap = strtoul(value, &end, 10);
            if (!errno && !*end && heap < 32)
                default_heap = (int)heap;
        }
        ion_fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
    }
    pthread_mutex_unlock(&mutex);
}

void rpcmem_deinit(void)
{
    pthread_mutex_lock(&mutex);
    if (ion_fd >= 0)
        close(ion_fd);
    ion_fd = -1;
    default_heap = -1;
    if (allocations)
        fprintf(stderr, "rpcmem: outstanding buffers retained at allocator shutdown\n");
    pthread_mutex_unlock(&mutex);
}

int rpcmem_to_fd_internal(void *pointer)
{
    int result = -1;
    pthread_mutex_lock(&mutex);
    for (struct allocation *item = allocations; item; item = item->next)
        if (item->buffer.data == pointer) {
            result = item->buffer.fd;
            break;
        }
    pthread_mutex_unlock(&mutex);
    return result;
}

void *rpcmem_alloc_internal(int heap, uint32_t flags, size_t size)
{
    const uint32_t supported =
        RPCMEM_HEAP_DEFAULT | RPCMEM_HEAP_NOREG | RPCMEM_HEAP_UNCACHED | RPCMEM_DEFAULT_FLAGS;
    if (!size || size > INT_MAX || (flags & ~supported)) {
        errno = EINVAL;
        return NULL;
    }
    struct allocation *item = calloc(1, sizeof(*item));
    if (!item)
        return NULL;
    item->buffer.fd = -1;
    item->requested_size = size;
    pthread_mutex_lock(&mutex);
    if (heap == RPCMEM_DEFAULT_HEAP || (flags & RPCMEM_HEAP_DEFAULT))
        heap = default_heap;
    int result;
    if (ion_fd < 0 || heap < 0 || heap >= 32) {
        result = -ENODEV;
    } else {
        int cached = !(flags & RPCMEM_HEAP_UNCACHED) &&
                     (flags & (RPCMEM_HEAP_DEFAULT | RPCMEM_DEFAULT_FLAGS));
        result = ion_buffer_allocate(ion_fd, (uint32_t)heap, size, cached, &item->buffer);
    }
    pthread_mutex_unlock(&mutex);
    if (result)
        goto fail;
    if (!item->buffer.data) {
        result = -ENOMEM;
        goto fail;
    }
    if (!(flags & RPCMEM_HEAP_NOREG)) {
        if (pico_rpcmem_register(item->buffer.data, size, item->buffer.fd)) {
            result = -EIO;
            goto fail;
        }
        item->registered = 1;
    }
    pthread_mutex_lock(&mutex);
    item->next = allocations;
    allocations = item;
    pthread_mutex_unlock(&mutex);
    return item->buffer.data;
fail:
    if (item->buffer.fd >= 0) {
        int released = ion_buffer_release(&item->buffer);
        if (released && item->buffer.fd >= 0) {
            pthread_mutex_lock(&mutex);
            item->next = allocations;
            allocations = item;
            pthread_mutex_unlock(&mutex);
            errno = -result;
            return NULL;
        }
    }
    free(item);
    errno = -result;
    return NULL;
}

void rpcmem_free_internal(void *pointer)
{
    struct allocation *item = NULL;
    pthread_mutex_lock(&mutex);
    for (struct allocation **link = &allocations; *link; link = &(*link)->next)
        if ((*link)->buffer.data == pointer) {
            item = *link;
            *link = item->next;
            break;
        }
    pthread_mutex_unlock(&mutex);
    if (!item)
        return;
    int result = 0;
    if (item->registered) {
        result = pico_rpcmem_register(item->buffer.data, item->requested_size, -1);
        if (!result)
            item->registered = 0;
    }
    if (!result)
        result = ion_buffer_release(&item->buffer);
    if (result && item->buffer.fd >= 0) {
        pthread_mutex_lock(&mutex);
        item->next = allocations;
        allocations = item;
        pthread_mutex_unlock(&mutex);
        fprintf(stderr, "rpcmem: failed release retained buffer ownership (%d)\n", result);
        return;
    }
    free(item);
}

void *rpcmem_alloc(int heap, uint32_t flags, int size)
{
    if (size <= 0) {
        errno = EINVAL;
        return NULL;
    }
    return rpcmem_alloc_internal(heap, flags, (size_t)size);
}
void *rpcmem_alloc2(int heap, uint32_t flags, size_t size)
{
    return rpcmem_alloc_internal(heap, flags, size);
}
void rpcmem_free(void *pointer) { rpcmem_free_internal(pointer); }
int rpcmem_to_fd(void *pointer) { return rpcmem_to_fd_internal(pointer); }
void rpcmem_init_internal(void) { rpcmem_init(); }
void rpcmem_deinit_internal(void) { rpcmem_deinit(); }
