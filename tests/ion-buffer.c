#define ioctl mock_ioctl
#define fcntl mock_fcntl
#define mmap mock_mmap
#define munmap mock_munmap
#define close mock_close
#include "../src/ion-buffer.c"
#undef ioctl
#undef fcntl
#undef mmap
#undef munmap
#undef close
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int allocation_error, descriptor_error, mapping_error, sync_error, unmap_error, close_error;
static int descriptor_flags = FD_CLOEXEC;
static uint32_t next_fd = 17;
static unsigned int calls, closes, maps, unmaps;
static struct ion_allocation last_allocation;
static uint64_t last_sync;
static unsigned char mapping[4096];

int mock_ioctl(int fd, unsigned long request, ...)
{
    ++calls;
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (request == ION_ALLOCATE) {
        assert(fd == 11);
        struct ion_allocation *allocation = arg;
        last_allocation = *allocation;
        if (allocation_error) {
            errno = allocation_error;
            return -1;
        }
        allocation->fd = next_fd;
    } else {
        assert(request == DMA_BUFFER_SYNC && fd == (int)next_fd);
        last_sync = *(uint64_t *)arg;
        if (sync_error) {
            errno = sync_error;
            return -1;
        }
    }
    return 0;
}
int mock_fcntl(int fd, int command, ...)
{
    assert(fd == (int)next_fd && command == F_GETFD);
    if (descriptor_error) {
        errno = descriptor_error;
        return -1;
    }
    return descriptor_flags;
}
void *mock_mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset)
{
    assert(!address && size == last_allocation.length && protection == (PROT_READ | PROT_WRITE));
    assert(flags == MAP_SHARED && fd == (int)next_fd && !offset);
    ++maps;
    if (mapping_error) {
        errno = mapping_error;
        return MAP_FAILED;
    }
    return mapping;
}
int mock_munmap(void *address, size_t size)
{
    assert(address == mapping && size == last_allocation.length);
    ++unmaps;
    if (unmap_error) {
        errno = unmap_error;
        return -1;
    }
    return 0;
}
int mock_close(int fd)
{
    assert(fd == (int)next_fd);
    ++closes;
    if (close_error) {
        errno = close_error;
        return -1;
    }
    return 0;
}

int main(void)
{
    struct ion_buffer buffer = {.fd = -1};
    assert(ion_buffer_allocate(-1, 25, 4096, true, &buffer) == -EINVAL);
    assert(ion_buffer_allocate(11, 32, 4096, true, &buffer) == -EINVAL);
    assert(ion_buffer_allocate(11, 25, 0, true, &buffer) == -EINVAL);
    assert(ion_buffer_allocate(11, 25, MAX_BUFFER_BYTES + 1, true, &buffer) == -EINVAL);
    assert(ion_buffer_allocate(11, 25, SIZE_MAX, true, &buffer) == -EINVAL);
    assert(ion_buffer_allocate(11, 25, 4096, true, NULL) == -EINVAL && !calls);
    for (unsigned int fail = 0; fail < 5; ++fail) {
        unsigned int before = closes;
        allocation_error = fail == 0 ? ENOMEM : 0;
        next_fd = fail == 1 ? UINT32_MAX : 17;
        descriptor_error = fail == 2 ? EBADF : 0;
        descriptor_flags = fail == 3 ? 0 : FD_CLOEXEC;
        mapping_error = fail == 4 ? ENOMEM : 0;
        int expected[] = {-ENOMEM, -EPROTO, -EBADF, -EPROTO, -ENOMEM};
        assert(ion_buffer_allocate(11, 25, 1, true, &buffer) == expected[fail]);
        assert(empty(&buffer) && closes == before + (fail >= 2));
    }
    allocation_error = descriptor_error = mapping_error = 0;
    descriptor_flags = FD_CLOEXEC;
    size_t lengths[] = {1, 4095, 4096, 4097, 851968, MAX_BUFFER_BYTES};
    for (unsigned int i = 0; i < sizeof(lengths) / sizeof(*lengths); ++i)
        for (unsigned int heap = 0; heap < 32; ++heap)
            for (unsigned int cached = 0; cached < 2; ++cached) {
                next_fd = i ? 17 : 0;
                assert(!ion_buffer_allocate(11, heap, lengths[i], cached, &buffer));
                assert(last_allocation.heap_mask == (UINT32_C(1) << heap));
                assert(last_allocation.flags == cached && !last_allocation.unused);
                assert(buffer.size == ((lengths[i] + 4095) & ~(size_t)4095));
                assert(buffer.fd == (int)next_fd && buffer.data == mapping);
                unsigned int before = calls;
                assert(ion_buffer_allocate(11, heap, 4096, cached, &buffer) == -EBUSY &&
                       calls == before);
                assert(ion_buffer_sync(&buffer, 0, false) == -EINVAL && calls == before);
                assert(ion_buffer_sync(&buffer, 4, false) == -EINVAL && calls == before);
                for (int access = 1; access <= 3; ++access)
                    for (int end = 0; end <= 1; ++end) {
                        assert(!ion_buffer_sync(&buffer, access, end));
                        assert(last_sync == (uint64_t)(access | (end ? 4 : 0)));
                    }
                sync_error = EAGAIN;
                assert(ion_buffer_sync(&buffer, ION_BUFFER_READ, false) == -EAGAIN);
                sync_error = 0;
                unmap_error = EBUSY;
                before = closes;
                assert(ion_buffer_release(&buffer) == -EBUSY && closes == before && owned(&buffer));
                unmap_error = 0;
                close_error = i == 0 ? EINTR : 0;
                assert(ion_buffer_release(&buffer) == (close_error ? -EINTR : 0));
                assert(empty(&buffer) && closes == before + 1);
                assert(!ion_buffer_release(&buffer) && closes == before + 1);
                before = calls;
                assert(ion_buffer_sync(&buffer, ION_BUFFER_READ, false) == -EINVAL &&
                       calls == before);
                close_error = 0;
            }
    assert(ion_buffer_release(NULL) == -EINVAL);
    puts("ION_BUFFER_UNIT_PASS allocations=384 failure_paths=5 sync_modes=6");
    return 0;
}
