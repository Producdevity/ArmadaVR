#define _GNU_SOURCE
#include "quest-camera-allocation.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef QUEST_CAMERA_VENDOR_TEST
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
_Static_assert(DMA_HEAP_IOCTL_ALLOC == 0xc0184800UL && DMA_BUF_IOCTL_SYNC == 0x40086200UL, "DMA heap/cache ABI");
_Static_assert(sizeof(struct dma_heap_allocation_data) == 24 && offsetof(struct dma_heap_allocation_data, fd) == 8, "DMA heap output");
_Static_assert((O_CLOEXEC | O_RDWR) == 0x80002 && DMA_BUF_SYNC_END == 4, "Linux DMA flags");
#endif
#ifdef QUEST_CAMERA_ALLOCATION_INSTALLED
#include <sys/syscall.h>
#define quest_test_ioctl ioctl
#define quest_test_fcntl fcntl
#define quest_test_mmap mmap
#define quest_test_munmap munmap
#define quest_test_close close
#endif
static unsigned calls, releases, closes, maps, unmaps, syncs, cycles, faults;
static int fail_allocate, fail_import, fail_flags, fail_map, fail_sync, fail_unmap, fail_close, fail_release;
static unsigned malformed;
static bool partial, live_handle, live_fd, mapped;
static uint8_t storage[8192];
static int dma_fd = 41;
static int32_t handle(void) { return (dma_fd << 16) | 7; }
static uint64_t get(const void *data, unsigned offset, unsigned bytes)
{
    uint64_t result = 0; const uint8_t *p = (const uint8_t *)data + offset;
    for (unsigned i = 0; i < bytes; ++i) result |= (uint64_t)p[i] << (i * 8);
    return result;
}
static void put(void *data, unsigned offset, uint64_t value, unsigned bytes)
{
    uint8_t *p = (uint8_t *)data + offset;
    for (unsigned i = 0; i < bytes; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
static int error(int code) { if (!code) return 0; errno = code; return code > 0 ? -1 : 1; }
int quest_test_ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request); void *p = va_arg(args, void *); va_end(args); ++calls;
    if (request == 0x40086200UL) {
        assert(fd == dma_fd && live_fd && mapped);
        uint64_t flags = *(uint64_t *)p;
        assert((flags & ~UINT64_C(7)) == 0 && (flags & 3)); ++syncs;
        return error(fail_sync);
    }
    if (request == 0xc0184800UL) {
        assert(fd == 32 && !live_fd && get(p, 0, 8) == 8192 && !get(p, 8, 4));
        assert(get(p, 12, 4) == 0x80002 && !get(p, 16, 8)); live_fd = true;
        if (!fail_allocate || partial) put(p, 8, malformed == 1 ? 32 : (unsigned)dma_fd, 4);
        return error(fail_allocate);
    }
    assert(request == 0xc01856c0UL && fd == 31 && get(p, 8, 4) == 1 && !get(p, 12, 4));
    void *data = (void *)(uintptr_t)get(p, 16, 8);
    unsigned op = get(p, 0, 4);
    if (op == 0x116) {
        assert(get(p, 4, 4) == 104 && !live_fd && !live_handle);
        assert(get(data, 0, 8) == 8192 && get(data, 8, 8) == 4096);
        live_handle = live_fd = true;
        if (!fail_allocate || partial) {
            put(data, 88, malformed == 2 ? 0 : (uint32_t)handle(), 4);
            put(data, 92, malformed == 1 ? 31 : (unsigned)dma_fd, 4);
        }
        return error(fail_allocate);
    }
    if (op == 0x117) {
        assert(get(p, 4, 4) == 96 && live_fd && !mapped && !live_handle);
        assert(get(data, 72, 4) == (unsigned)dma_fd); live_handle = true;
        if (!fail_import || partial) {
            put(data, 80, malformed == 2 ? 0 : (uint32_t)handle(), 4); put(data, 84, 8192, 4);
        }
        return error(fail_import);
    }
    assert(op == 0x118 && get(p, 4, 4) == 8 && get(data, 0, 4) == (uint32_t)handle());
    assert(live_handle && live_fd); ++releases; live_handle = false;
    return error(fail_release);
}
int quest_test_fcntl(int fd, int op, ...)
{ assert(fd == dma_fd && live_fd && op == F_GETFD); return fail_flags ? error(fail_flags) : malformed == 3 ? 0 : FD_CLOEXEC; }
void *quest_test_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset)
{
#ifdef QUEST_CAMERA_ALLOCATION_INSTALLED
    /* ASan also maps anonymous storage through the executable's interposition. */
    if (fd == -1 && (flags & MAP_ANONYMOUS))
        return (void *)syscall(SYS_mmap, address, size, prot, flags, fd, offset);
#endif
    assert(!address && size == sizeof(storage) && prot == (PROT_READ | PROT_WRITE));
    assert(flags == MAP_SHARED && fd == dma_fd && live_fd && !mapped && !offset); ++maps;
    if (fail_map) { errno = fail_map; return MAP_FAILED; }
    mapped = true; return malformed == 4 ? NULL : storage;
}
int quest_test_munmap(void *address, size_t size)
{
#ifdef QUEST_CAMERA_ALLOCATION_INSTALLED
    if (address != NULL && address != storage) return (int)syscall(SYS_munmap, address, size);
#endif
    assert(address == (malformed == 4 ? NULL : storage) && size == sizeof(storage) && mapped && !live_handle); ++unmaps;
    if (fail_unmap) return error(fail_unmap);
    mapped = false; return 0;
}
int quest_test_close(int fd)
{
    assert(fd == dma_fd && live_fd && !mapped && !live_handle); ++closes;
    live_fd = false; return error(fail_close);
}
static struct quest_camera_allocation_config config(enum quest_camera_allocation_kind kind)
{
    return (struct quest_camera_allocation_config){.kind = kind,
        .mapping = kind == QUEST_CAMERA_ALLOCATION_PACKET ? QUEST_CAMERA_MEMORY_CPU : QUEST_CAMERA_MEMORY_IO,
        .size = sizeof(storage), .mmus = {100}, .mmu_count = kind == QUEST_CAMERA_ALLOCATION_PACKET ? 0 : 1};
}
static void reset(void)
{
    assert(!live_handle && !live_fd && !mapped);
    calls = releases = closes = maps = unmaps = syncs = 0;
    fail_allocate = fail_import = fail_flags = fail_map = fail_sync = fail_unmap = fail_close = fail_release = 0;
    partial = false; malformed = 0; dma_fd = 41;
}
static void finish(struct quest_camera_allocation **b)
{
    assert(!quest_camera_allocation_release(b) && !*b && !live_handle && !live_fd && !mapped);
    assert(!quest_camera_allocation_release(b));
}
static void dispose_uncertain(struct quest_camera_allocation **b, int expected)
{
    unsigned before = calls + unmaps + closes;
    assert(quest_camera_allocation_release(b) == expected && calls + unmaps + closes == before);
    struct quest_camera_allocation_view view;
    assert(quest_camera_allocation_resources(*b, &view));
    live_handle = live_fd = mapped = false;
    quest_camera_allocation_closed(b); assert(!*b);
}
int main(void)
{
    struct quest_camera_allocation *b = NULL; struct quest_camera_allocation_view view;
    struct quest_camera_allocation_config c = config(QUEST_CAMERA_ALLOCATION_PACKET);
    assert(quest_camera_allocation_create(-1, -1, &c, &b) == -EINVAL);
    assert(quest_camera_allocation_create(31, -1, NULL, &b) == -EINVAL);
    c.size = 1; assert(quest_camera_allocation_create(31, -1, &c, &b) == -EINVAL);
    c = config(QUEST_CAMERA_ALLOCATION_PIXELS); c.mmus[0] = -1;
    assert(quest_camera_allocation_create(31, 32, &c, &b) == -EINVAL);
    c = config(QUEST_CAMERA_ALLOCATION_PIXELS); c.mmu_count = 2; c.mmus[1] = 100;
    assert(quest_camera_allocation_create(31, 32, &c, &b) == -EINVAL && !calls && !b);
    for (unsigned cycle = 0; cycle < 200; ++cycle) for (unsigned kind = 0; kind < 3; ++kind) {
        reset(); dma_fd = cycle % 3 == 0 ? 0 : cycle % 3 == 1 ? 41 : 32767; c = config(kind);
        assert(!quest_camera_allocation_create(31, 32, &c, &b));
        assert(quest_camera_allocation_view(b, &view) && view.handle == handle() && view.fd == dma_fd);
        assert(view.data == storage && view.size == sizeof(storage) && view.mapped);
        assert(!quest_camera_allocation_cpu_begin(b, QUEST_CAMERA_CPU_WRITE)); memset(view.data, 0x5a, view.size);
        assert(quest_camera_allocation_submit(b) == -EBUSY && quest_camera_allocation_release(&b) == -EBUSY);
        assert(!quest_camera_allocation_cpu_end(b) && !quest_camera_allocation_submit(b));
        assert(quest_camera_allocation_submit(b) == -EBUSY && quest_camera_allocation_release(&b) == -EBUSY);
        assert(quest_camera_allocation_cpu_begin(b, QUEST_CAMERA_CPU_READ) == -EBUSY);
        assert(!quest_camera_allocation_complete(b) && quest_camera_allocation_complete(b) == -EINVAL);
        assert(!quest_camera_allocation_cpu_begin(b, QUEST_CAMERA_CPU_READ)); assert(storage[8191] == 0x5a);
        assert(!quest_camera_allocation_cpu_end(b));
        assert(quest_camera_allocation_release(&b) == -EBUSY);
        assert(!quest_camera_allocation_quiesced(b));
        assert(!quest_camera_allocation_view(b, &view) && quest_camera_allocation_submit(b) == -EINVAL);
        finish(&b); ++cycles;
    }
    for (unsigned kind = 0; kind < 3; ++kind) for (unsigned fault = 0; fault < 16; ++fault) {
        reset(); c = config(kind);
        if (fault < 2) { fail_allocate = EINTR; partial = fault; }
        if (fault == 2) fail_flags = EBADF;
        if (fault == 3) fail_map = ENOMEM;
        if (fault >= 4 && fault <= 7) malformed = fault - 3;
        if (fault == 14) { fail_allocate = EINTR; partial = true; dma_fd = 0; }
        if (fault == 15) { fail_allocate = -1; partial = true; }
        int result = quest_camera_allocation_create(31, 32, &c, &b);
        if (fault < 8 || fault >= 14) assert(result && b && !quest_camera_allocation_view(b, &view));
        else {
            assert(!result && b);
            if (fault < 10) {
                if (fault == 9) assert(!quest_camera_allocation_cpu_begin(b, QUEST_CAMERA_CPU_WRITE));
                fail_sync = EINTR;
                assert((fault == 8 ? quest_camera_allocation_cpu_begin(b, QUEST_CAMERA_CPU_READ) : quest_camera_allocation_cpu_end(b)) == -EINTR);
                unsigned count = syncs;
                assert(quest_camera_allocation_cpu_begin(b, QUEST_CAMERA_CPU_READ) == -EINVAL && syncs == count);
            } else if (fault == 10) fail_release = EINTR;
            else if (fault == 11) fail_unmap = EINVAL;
            else if (fault == 12) fail_close = EINTR;
            else assert(!quest_camera_allocation_submit(b));
        }
        assert(!quest_camera_allocation_quiesced(b));
        result = quest_camera_allocation_release(&b);
        bool uncertain = fault == 0 || fault == 4 || fault == 5 || (kind == 2 && fault == 14);
        if (uncertain || fault == 10 || fault == 11) {
            assert(b && result < 0); dispose_uncertain(&b, result);
        } else {
            assert(!b && result == (fault == 12 ? -EINTR : 0));
            assert(!live_handle && !live_fd && !mapped);
        }
        ++faults;
    }
    for (unsigned copy = 0; copy < 2; ++copy) {
        reset(); c = config(QUEST_CAMERA_ALLOCATION_PIXELS); fail_import = EFAULT; partial = copy;
        assert(quest_camera_allocation_create(31, 32, &c, &b) == -EFAULT && b);
        assert(quest_camera_allocation_release(&b) == -EBUSY && !quest_camera_allocation_quiesced(b));
        if (copy) finish(&b);
        else { assert(quest_camera_allocation_release(&b) == -EIO); dispose_uncertain(&b, -EIO); }
        ++faults;
    }
    reset(); c = config(QUEST_CAMERA_ALLOCATION_PIXELS); dma_fd = 32768;
    assert(quest_camera_allocation_create(31, 32, &c, &b) == -ERANGE && b && !live_handle);
    assert(!quest_camera_allocation_quiesced(b)); finish(&b); ++faults;
    reset(); c = config(QUEST_CAMERA_ALLOCATION_PACKET);
#define BAD(field, value) do { struct quest_camera_allocation_config bad = c; bad.field = value; \
    assert(quest_camera_allocation_create(31, -1, &bad, &b) == -EINVAL && !b && !calls); } while (0)
    BAD(kind, -1); BAD(kind, 3); BAD(size, 0); BAD(size, SIZE_MAX); BAD(mmu_count, 17);
    BAD(mapping, -1); BAD(mapping, 3); BAD(mapping, QUEST_CAMERA_MEMORY_IO); BAD(mmu_count, 1);
    assert(!quest_camera_allocation_create(31, -1, &c, &b)); finish(&b);
    printf("QUEST_CAMERA_ALLOCATION_PASS cycles=%u faults=%u hardware=0\n", cycles, faults);
}
