#define ioctl mock_ioctl
#define fcntl mock_fcntl
#define mmap mock_mmap
#define munmap mock_munmap
#define close mock_close
#include "../src/ion-buffer.c"
#include "../src/pico-camera-control.c"
#include "../src/pico-camera-buffer.c"
#undef ioctl
#undef fcntl
#undef mmap
#undef munmap
#undef close
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned calls, releases, closes, maps, unmaps, syncs;
static unsigned fail_allocation, fail_import, fail_map, fail_sync, fail_unmap, fail_close, fail_release;
static bool partial, live_handle, live_fd, mapped;
static uint8_t storage[8192];
static const int dma_fd = 41;
static const int32_t handle = (41 << 16) | 7;
static struct pico_camera_buffer *retained_failure;
static uint64_t read_value(const void *data, unsigned offset, unsigned bytes)
{
    uint64_t result = 0; const uint8_t *p = (const uint8_t *)data + offset;
    for (unsigned i = 0; i < bytes; ++i) result |= (uint64_t)p[i] << (i * 8);
    return result;
}
static void write_value(void *data, unsigned offset, uint64_t value, unsigned bytes)
{
    uint8_t *p = (uint8_t *)data + offset;
    for (unsigned i = 0; i < bytes; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
static int error(unsigned code) { if (!code) return 0; errno = (int)code; return -1; }
int mock_ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request); void *p = va_arg(args, void *); va_end(args); ++calls;
    if (request == DMA_BUFFER_SYNC) {
        assert(fd == dma_fd && live_fd && mapped);
        uint64_t flags = *(uint64_t *)p;
        assert((flags & ~UINT64_C(7)) == 0 && (flags & 3)); ++syncs;
        return error(fail_sync);
    }
    if (request == ION_ALLOCATE) {
        assert(fd == 32 && !live_fd);
        if (fail_allocation) return error(fail_allocation);
        ((struct ion_allocation *)p)->fd = dma_fd; live_fd = true;
        return 0;
    }
    assert(request == 0xc01856c0UL && fd == 31);
    struct control_command *c = p; assert(c->handle_type == 1 && !c->reserved);
    void *data = (void *)(uintptr_t)c->pointer;
    if (c->opcode == 0x112) {
        assert(c->size == 104 && !live_fd && !live_handle);
        if (!fail_allocation || partial) {
            write_value(data, 88, handle, 4); write_value(data, 92, dma_fd, 4);
            live_handle = live_fd = true;
        }
        return error(fail_allocation);
    }
    if (c->opcode == 0x113) {
        assert(c->size == 96 && live_fd && mapped && !live_handle);
        assert(read_value(data, 72, 4) == dma_fd);
        if (!fail_import || partial) { write_value(data, 80, handle, 4); live_handle = true; }
        return error(fail_import);
    }
    assert(c->opcode == 0x114 && c->size == 8 && read_value(data, 0, 4) == (uint32_t)handle);
    assert(live_handle && live_fd); ++releases;
    live_handle = false;
    return error(fail_release);
}
int mock_fcntl(int fd, int op, ...) { assert(fd == dma_fd && live_fd && op == F_GETFD); return FD_CLOEXEC; }
void *mock_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset)
{
    assert(!address && size == sizeof(storage) && prot == (PROT_READ | PROT_WRITE));
    assert(flags == MAP_SHARED && fd == dma_fd && live_fd && !mapped && !offset); ++maps;
    if (fail_map) { errno = (int)fail_map; return MAP_FAILED; }
    mapped = true; return storage;
}
int mock_munmap(void *address, size_t size)
{
    assert(address == storage && size == sizeof(storage) && mapped && !live_handle); ++unmaps;
    if (fail_unmap) return error(fail_unmap);
    mapped = false; return 0;
}
int mock_close(int fd)
{
    assert(fd == dma_fd && live_fd && !mapped && !live_handle); ++closes;
    live_fd = false; return error(fail_close);
}
static struct pico_camera_buffer_config config(enum pico_camera_buffer_kind kind)
{
    return (struct pico_camera_buffer_config){.kind = kind,
        .mapping = kind == PICO_CAMERA_BUFFER_PACKET ? PICO_CAMERA_MEMORY_CPU : PICO_CAMERA_MEMORY_IO,
        .size = sizeof(storage), .mmus = {100}, .mmu_count = kind == PICO_CAMERA_BUFFER_PACKET ? 0 : 1,
        .ion_heap = 25, .cached = true};
}
static void finish(struct pico_camera_buffer **b)
{
    assert(!pico_camera_buffer_release(b) && !*b && !live_handle && !live_fd && !mapped);
    assert(!pico_camera_buffer_release(b));
}
int main(void)
{
    struct pico_camera_buffer *b = NULL; struct pico_camera_buffer_view view = {0};
    struct pico_camera_buffer_config c = config(PICO_CAMERA_BUFFER_PACKET);
    assert(pico_camera_buffer_create(-1, -1, &c, &b) == -EINVAL);
    assert(pico_camera_buffer_create(31, -1, NULL, &b) == -EINVAL);
    c.size = 1; assert(pico_camera_buffer_create(31, -1, &c, &b) == -EINVAL);
    c = config(PICO_CAMERA_BUFFER_PIXELS); c.mmus[0] = -1;
    assert(pico_camera_buffer_create(31, 32, &c, &b) == -EINVAL);
    c = config(PICO_CAMERA_BUFFER_PIXELS); c.mmu_count = 2; c.mmus[1] = 100;
    assert(pico_camera_buffer_create(31, 32, &c, &b) == -EINVAL && !calls && !b);
    for (unsigned cycle = 0; cycle < 200; ++cycle) for (unsigned kind = 0; kind < 3; ++kind) {
        c = config((enum pico_camera_buffer_kind)kind);
        assert(!pico_camera_buffer_create(31, 32, &c, &b));
        assert(pico_camera_buffer_view(b, &view) && view.memory_handle == handle && view.fd == dma_fd);
        assert(view.data == storage && view.size == sizeof(storage));
        assert(!pico_camera_buffer_cpu_begin(b, ION_BUFFER_WRITE)); memset(view.data, 0x5a, view.size);
        assert(pico_camera_buffer_submit(b) == -EBUSY && pico_camera_buffer_release(&b) == -EBUSY);
        assert(!pico_camera_buffer_cpu_end(b));
        assert(!pico_camera_buffer_submit(b));
        assert(pico_camera_buffer_submit(b) == -EBUSY && pico_camera_buffer_release(&b) == -EBUSY);
        assert(pico_camera_buffer_cpu_begin(b, ION_BUFFER_READ) == -EBUSY);
        assert(!pico_camera_buffer_complete(b));
        assert(pico_camera_buffer_complete(b) == -EINVAL);
        assert(!pico_camera_buffer_cpu_begin(b, ION_BUFFER_READ)); assert(storage[8191] == 0x5a);
        assert(!pico_camera_buffer_cpu_end(b)); finish(&b);
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        partial = mode; fail_allocation = EINTR; c = config(PICO_CAMERA_BUFFER_COMMAND);
        assert(pico_camera_buffer_create(31, -1, &c, &b) == -EINTR && b);
        assert(!pico_camera_buffer_view(b, &view)); finish(&b); fail_allocation = 0;
        fail_import = EIO; c = config(PICO_CAMERA_BUFFER_PIXELS);
        assert(pico_camera_buffer_create(31, 32, &c, &b) == -EIO && b);
        finish(&b); fail_import = 0;
    }
    partial = false; fail_map = ENOMEM; c = config(PICO_CAMERA_BUFFER_PACKET);
    assert(pico_camera_buffer_create(31, -1, &c, &b) == -ENOMEM && b);
    finish(&b); fail_map = 0;
    for (unsigned end = 0; end < 2; ++end) {
        assert(!pico_camera_buffer_create(31, -1, &c, &b));
        if (end) assert(!pico_camera_buffer_cpu_begin(b, ION_BUFFER_READ));
        fail_sync = EIO;
        assert((end ? pico_camera_buffer_cpu_end(b) : pico_camera_buffer_cpu_begin(b, ION_BUFFER_READ)) == -EIO);
        assert(!pico_camera_buffer_view(b, &view) && pico_camera_buffer_submit(b) == -EINVAL);
        fail_sync = 0; finish(&b);
    }
    assert(!pico_camera_buffer_create(31, -1, &c, &b));
    fail_unmap = EBUSY; unsigned before = releases;
    assert(pico_camera_buffer_release(&b) == -EBUSY && b && !live_handle && mapped);
    fail_unmap = 0; finish(&b); assert(releases == before + 1);
    assert(!pico_camera_buffer_create(31, -1, &c, &b)); fail_close = EINTR; before = closes;
    assert(pico_camera_buffer_release(&b) == -EINTR && !b && !live_fd && closes == before + 1);
    assert(!pico_camera_buffer_release(&b) && closes == before + 1); fail_close = 0;
    assert(!pico_camera_buffer_create(31, -1, &c, &b)); fail_release = EIO; before = releases;
    assert(pico_camera_buffer_release(&b) == -EIO && b && live_fd && mapped && !live_handle);
    assert(pico_camera_buffer_release(&b) == -EIO && releases == before + 1);
    assert(!pico_camera_buffer_view(b, &view) && pico_camera_buffer_submit(b) == -EINVAL);
    retained_failure = b;
    printf("PICO_CAMERA_BUFFER_PASS cycles=600 faults=10 retained_uncertain=1 maps=%u unmaps=%u syncs=%u\n", maps, unmaps, syncs);
    return retained_failure ? 0 : 1;
}
