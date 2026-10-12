#include "ion-buffer.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct ion_allocation {
    uint64_t length;
    uint32_t heap_mask, flags, fd, unused;
};
_Static_assert(sizeof(struct ion_allocation) == 24, "Pico ION allocation ABI");
_Static_assert(offsetof(struct ion_allocation, fd) == 16, "Pico ION fd offset");

#define ION_ALLOCATE 0xc0184900UL
#define DMA_BUFFER_SYNC 0x40086200UL
#define PAGE_BYTES 4096U
#define MAX_BUFFER_BYTES ((size_t)INT32_MAX & ~(size_t)(PAGE_BYTES - 1))

static bool empty(const struct ion_buffer *buffer)
{
    return buffer->fd == -1 && !buffer->data && !buffer->size;
}

static bool owned(const struct ion_buffer *buffer)
{
    return buffer && buffer->fd >= 0 && buffer->size && buffer->size <= MAX_BUFFER_BYTES &&
           !(buffer->size % PAGE_BYTES);
}

int ion_buffer_allocate(int ion_fd, uint32_t heap_id, size_t size, bool cached,
                        struct ion_buffer *buffer)
{
    if (!buffer || ion_fd < 0 || heap_id >= 32 || !size || size > MAX_BUFFER_BYTES)
        return -EINVAL;
    if (!empty(buffer))
        return -EBUSY;
    size = (size + PAGE_BYTES - 1) & ~(size_t)(PAGE_BYTES - 1);
    struct ion_allocation request = {
        .length = size,
        .heap_mask = UINT32_C(1) << heap_id,
        .flags = cached ? 1U : 0U,
        .fd = UINT32_MAX,
    };
    if (ioctl(ion_fd, ION_ALLOCATE, &request) < 0)
        return -errno;
    if (request.fd > INT_MAX)
        return -EPROTO;
    int fd = (int)request.fd;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || !(flags & FD_CLOEXEC)) {
        int error = flags < 0 ? errno : EPROTO;
        close(fd);
        return -error;
    }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        int error = errno;
        close(fd);
        return -error;
    }
    *buffer = (struct ion_buffer){.fd = fd, .data = data, .size = size};
    return 0;
}

int ion_buffer_sync(const struct ion_buffer *buffer, enum ion_buffer_access access, bool end)
{
    if (!owned(buffer) || access < ION_BUFFER_READ || access > ION_BUFFER_READ_WRITE)
        return -EINVAL;
    uint64_t flags = (uint64_t)access | (end ? UINT64_C(4) : 0);
    return ioctl(buffer->fd, DMA_BUFFER_SYNC, &flags) < 0 ? -errno : 0;
}

int ion_buffer_release(struct ion_buffer *buffer)
{
    if (!buffer)
        return -EINVAL;
    if (empty(buffer))
        return 0;
    if (!owned(buffer))
        return -EINVAL;
    if (munmap(buffer->data, buffer->size))
        return -errno;
    int result = close(buffer->fd);
    int error = errno;
    *buffer = (struct ion_buffer){.fd = -1};
    return result < 0 ? -error : 0;
}
