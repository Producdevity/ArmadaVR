#define _GNU_SOURCE
#include "quest-camera-front-end.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef QUEST_CAMERA_FRONT_END_INSTALLED
#include <sys/syscall.h>
#define quest_test_ioctl ioctl
#define quest_test_fcntl fcntl
#define quest_test_mmap mmap
#define quest_test_munmap munmap
#define quest_test_close close
#endif

static struct { unsigned char data[8192]; size_t size; bool fd, handle, mapped; } buffers[3];
static unsigned next_buffer, operations, fail_at, submitted, variant, cycles, faults;
static unsigned phase, rdi, width, height, pattern, hardware_index;
static bool ife, shared;
static bool partial, lost, quiesced;
static uint64_t get(const void *data, unsigned offset, unsigned size)
{
    const unsigned char *p = (const unsigned char *)data + offset; uint64_t v = 0;
    for (unsigned i = 0; i < size; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void put(void *data, unsigned offset, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) ((unsigned char *)data)[offset + i] = (unsigned char)(value >> (8 * i)); }
static bool failure(void) { ++operations; return operations == fail_at; }
static int failed(void) { errno = EINTR; return -1; }
static uint32_t handle(unsigned i) { return ((100 + i) << 16) | (i + 1); }
static unsigned index_of_handle(uint32_t value)
{
    unsigned i = (value & 0x7fff) - 1; assert(i < next_buffer && value == handle(i)); return i;
}
static void verify_packet(unsigned i)
{
    assert(!submitted && i == (ife ? 2U : 1U));
    const unsigned char *packet = buffers[i].data, *direct = buffers[0].data;
    assert(buffers[i].mapped && buffers[0].mapped && buffers[i].handle && buffers[0].handle);
    assert(get(packet, 0, 4) == (ife ? 0x0f000000U : 0x05000000U));
    assert(get(packet, 4, 4) == (ife ? 104U : 80U) && !get(packet, 8, 8));
    assert(get(packet, 28, 4) == (ife ? 2U : 1U) && !get(packet, 36, 4));
    assert(get(packet, 48, 4) == (ife ? 0U : UINT32_MAX) && get(packet, 52, 4) == (ife ? 56U : 0U));
    assert(get(packet, 56, 4) == handle(0) && !get(packet, 60, 4));
    assert(get(packet, 64, 4) == (ife ? 8192U : 4096U) && get(packet, 68, 4) == (ife ? 56U : 24U));
    assert(get(packet, 72, 4) == 5 && get(packet, 76, 4) == (ife ? 3U : 0U));
    unsigned length;
    if (ife) {
        assert(get(direct, 0, 4) == 0x03000001 && get(direct, 4, 4) == 0x1060 && get(direct, 8, 4) == 0x1f);
        assert(get(direct, 12, 4) == 0x03000001 && get(direct, 16, 4) == 0x528 + rdi * 256);
        assert(get(direct, 20, 4) == pattern << 24);
        assert(get(direct, 24, 4) == 0x03000002 && get(direct, 28, 4) == 0x548 + rdi * 256);
        assert(get(direct, 32, 4) == 1 && !get(direct, 36, 4));
        assert(get(direct, 40, 4) == 0x03000002 && get(direct, 44, 4) == 0x550 + rdi * 256);
        assert(get(direct, 48, 4) == (width - 1) << 16 && get(direct, 52, 4) == (height - 1) << 16);
        assert(get(packet, 80, 4) == handle(1) && !get(packet, 84, 4) && get(packet, 88, 4) == 4096);
        assert(get(packet, 92, 4) == 176 && get(packet, 96, 4) == 9 && get(packet, 100, 4) == 12);
        const unsigned char *g = buffers[1].data;
        assert(get(g, 0, 4) == 32 << 8 && get(g, 4, 4) == 1 && !get(g, 8, 4));
        assert(get(g, 12, 4) == 0x3006 + rdi && get(g, 16, 4) == 1 && !get(g, 20, 4));
        assert(get(g, 24, 4) == 1 && !get(g, 28, 8)); g += 36;
        assert(get(g, 0, 4) == (56 << 8 | 1) && !get(g, 4, 4) && get(g, 8, 4) == 4);
        assert(get(g, 12, 8) == 200000000 && !get(g, 20, 8));
        for (unsigned n = 0; n < 4; ++n) assert(get(g, 28 + n * 8, 8) == (n == rdi ? 100000000U : 0U));
        g += 60;
        assert(get(g, 0, 4) == (64 << 8 | 9) && !get(g, 4, 4) && get(g, 8, 4) == 1);
        assert(get(g, 12, 4) == 3 && get(g, 16, 4) == 1 && get(g, 20, 4) == 4 + rdi && !get(g, 24, 4));
        assert(get(g, 28, 8) == 1000000 && get(g, 36, 8) == 2000000 && get(g, 44, 8) == 3000000);
        assert(get(g, 52, 8) == 4000000 && get(g, 60, 8) == 5000000); g += 68;
        assert(get(g, 0, 4) == (8 << 8 | 4) && get(g, 4, 8) == 80000000);
        for (unsigned n = 176; n < 4096; ++n) assert(!buffers[1].data[n]);
        length = 56;
    } else {
        assert(!get(direct, 0, 2) && get(direct, 2, 2) == (variant ? 0x210U : 0x3210U));
        assert(get(direct, 4, 2) == 6 && get(direct, 6, 2) == (variant ? 3U : 4U));
        assert(get(direct, 8, 8) == 8000000000ULL && get(direct, 16, 8) == 400000000);
        length = 24;
    }
    for (unsigned n = length; n < buffers[0].size; ++n) assert(!direct[n]);
    for (unsigned n = ife ? 104 : 80; n < 4096; ++n) assert(!packet[n]);
}
int quest_test_ioctl(int fd, unsigned long request, ...)
{
    assert(request == 0xc01856c0UL || request == 0x40086200UL);
    va_list ap; va_start(ap, request); void *p = va_arg(ap, void *); va_end(ap);
    bool fail = failure();
    if (request == 0x40086200UL) {
        assert(!phase && fd >= 100 && (unsigned)fd < 100 + next_buffer);
        assert(buffers[fd - 100].mapped && buffers[fd - 100].fd);
        assert(get(p, 0, 8) == 2 || get(p, 0, 8) == 6);
        return fail ? failed() : 0;
    }
    unsigned op = get(p, 0, 4); assert(get(p, 8, 4) == 1 && !get(p, 12, 4));
    void *data = (void *)(uintptr_t)get(p, 16, 8);
    if (op == 0x116) {
        assert(!phase && fd == 31 && get(p, 4, 4) == 104 && next_buffer < 3);
        unsigned i = next_buffer++;
        bool mapped = ife && !i, packet = i == (ife ? 2U : 1U);
        buffers[i].size = mapped ? 8192 : 4096;
        assert(get(data, 0, 8) == buffers[i].size && get(data, 8, 8) == 4096);
        assert(get(data, 80, 4) == (mapped ? 1U : 0U) && get(data, 16, 4) == (mapped ? 55U : 0U));
        unsigned flags = 8 | 16 | 8192 | 1024 | (packet ? 512 : 64) | (mapped ? 1 : 0) | (mapped && shared ? 2048 : 0);
        assert(get(data, 84, 4) == flags);
        buffers[i].fd = buffers[i].handle = true;
        if (!fail || partial) { put(data, 88, handle(i)); put(data, 92, 100 + i); }
        else lost = true;
    } else if (op == 0x105) {
        assert(phase == 1 && fd == 32 && get(p, 4, 4) == 24);
        assert(get(data, 0, 4) == 41 && get(data, 4, 4) == 42 && !get(data, 8, 8));
        unsigned i = index_of_handle(get(data, 16, 8)); verify_packet(i); ++submitted;
    } else {
        assert(phase == 2 && quiesced && fd == 31 && op == 0x118 && get(p, 4, 4) == 8);
        unsigned i = index_of_handle(get(data, 0, 4)); assert(buffers[i].fd && buffers[i].handle);
        buffers[i].handle = false;
    }
    return fail ? failed() : 0;
}
int quest_test_fcntl(int fd, int op, ...)
{
    assert(!phase && fd >= 100 && (unsigned)fd < 100 + next_buffer && buffers[fd - 100].fd && op == F_GETFD);
    return failure() ? failed() : FD_CLOEXEC;
}
void *quest_test_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset)
{
#ifdef QUEST_CAMERA_FRONT_END_INSTALLED
    /* ASan also maps anonymous storage through the executable's interposition. */
    if (fd == -1 && (flags & MAP_ANONYMOUS))
        return (void *)syscall(SYS_mmap, address, size, prot, flags, fd, offset);
#endif
    assert(!phase && !address && prot == (PROT_READ | PROT_WRITE) && flags == MAP_SHARED && !offset);
    assert(fd >= 100 && (unsigned)fd < 100 + next_buffer && buffers[fd - 100].fd);
    assert(size == buffers[fd - 100].size);
    if (failure()) { errno = EINTR; return MAP_FAILED; }
    buffers[fd - 100].mapped = true; memset(buffers[fd - 100].data, 0xa5, sizeof(buffers[fd - 100].data));
    return buffers[fd - 100].data;
}
int quest_test_munmap(void *address, size_t size)
{
#ifdef QUEST_CAMERA_FRONT_END_INSTALLED
    if ((uintptr_t)address < (uintptr_t)buffers || (uintptr_t)address >= (uintptr_t)(buffers + 3)) return (int)syscall(SYS_munmap, address, size);
#endif
    assert(phase == 2 && quiesced);
    unsigned i = 0; while (i < next_buffer && address != buffers[i].data) ++i;
    assert(i < next_buffer && size == buffers[i].size && buffers[i].mapped && !buffers[i].handle);
    if (failure()) return failed();
    buffers[i].mapped = false; return 0;
}
int quest_test_close(int fd)
{
    assert(phase == 2 && quiesced && fd >= 100 && (unsigned)fd < 100 + next_buffer);
    unsigned i = fd - 100; assert(buffers[i].fd && !buffers[i].mapped && !buffers[i].handle);
    buffers[i].fd = false; return failure() ? failed() : 0;
}
static struct quest_camera_ife_setup configuration(void)
{
    const uint32_t ids[] = {4,8,16,32,64,128,65536,131072,262144,524288};
    struct quest_camera_ife_setup c = {.target = {31,32,41,42},
        .hardware = {.id = {ids[hardware_index]}, .paths = {{8U << rdi}}, .valid = 1},
        .input = {QUEST_CAMERA_IFE_TITAN636,rdi,pattern,width,height}, .command_mmu = 55,
        .command_mapping = shared ? QUEST_CAMERA_MEMORY_SHARED : QUEST_CAMERA_MEMORY_IO,
        .clocks = {rdi,200000000,100000000}, .bandwidth = {rdi,1000000,2000000,3000000,4000000,5000000},
        .csid_clock_hz = 80000000};
    return c;
}
static void run(unsigned prepare_error, bool copy, unsigned configure_error, unsigned cleanup_error, bool cancel)
{
    for (unsigned i = 0; i < 3; ++i) assert(!buffers[i].fd && !buffers[i].handle && !buffers[i].mapped);
    next_buffer = operations = submitted = phase = 0;
    fail_at = prepare_error; partial = copy; lost = quiesced = false;
    ife = variant > 1; shared = variant == 3;
    rdi = cycles % 4; pattern = (cycles / 4) % 8; hardware_index = (cycles / 4) % 10;
    width = 640 + cycles % 64; height = 513 + cycles % 32;
    struct quest_camera_ife_setup c = configuration();
    struct quest_camera_phy_config phy = {variant ? 1U : 0U,0,variant ? 3U : 4U,variant ? 0x210U : 0x3210U,40,6,400000000};
    struct quest_camera_front_end *s = NULL;
    int result = ife ? quest_camera_front_end_prepare_ife(&c, &s) : quest_camera_front_end_prepare_phy(&c.target, &phy, &s);
    assert(s && result == (prepare_error ? -EINTR : 0));
    unsigned before = operations;
    if (prepare_error) assert(quest_camera_front_end_submit(s) == -EINVAL && operations == before);
    else {
        assert(operations == (ife ? 15U : 10U));
        memset(&phy, 0xa5, sizeof(phy)); memset(&c, 0xa5, sizeof(c));
        phase = 1; operations = 0; fail_at = configure_error;
        if (!cancel) {
            result = quest_camera_front_end_submit(s);
            assert(result == (configure_error ? -EINTR : 0) && submitted == 1);
            before = operations;
            assert(quest_camera_front_end_submit(s) == -EINVAL && operations == before);
        }
    }
    before = operations;
    if (!cancel) assert(quest_camera_front_end_release(&s) == -EBUSY && before == operations);
    if (!cancel) assert(!quest_camera_front_end_quiesced(s) && quest_camera_front_end_quiesced(s) == -EINVAL);
    quiesced = true; phase = 2; operations = 0; fail_at = cleanup_error;
    result = quest_camera_front_end_release(&s);
    if (lost || cleanup_error) {
        assert(s && result == (lost ? -EIO : -EINTR)); before = operations;
        assert(quest_camera_front_end_release(&s) == result && operations == before);
        struct quest_camera_allocation_view view;
        for (unsigned i = 0; i < 3; ++i)
            if (quest_camera_front_end_resources(s, i, &view)) {
                assert(view.size == (ife && !i ? 8192U : 4096U));
                if (view.mapped) assert(view.data != NULL && view.fd >= 100);
            }
        for (unsigned i = 0; i < next_buffer; ++i) buffers[i].fd = buffers[i].handle = buffers[i].mapped = false;
        quest_camera_front_end_closed(&s); assert(!s && before == operations);
    } else {
        assert(!result && !s);
        for (unsigned i = 0; i < next_buffer; ++i) assert(!buffers[i].fd && !buffers[i].handle && !buffers[i].mapped);
    }
    assert(!quest_camera_front_end_release(&s));
    if (prepare_error || configure_error || cleanup_error) ++faults; else ++cycles;
}
int main(void)
{
    for (variant = 0; variant < 4; ++variant) {
        unsigned count = variant > 1 ? 3 : 2;
        for (unsigned i = 0; i < 200; ++i) run(0, true, 0, 0, false);
        for (unsigned i = 1; i <= count * 5; ++i) { run(i, true, 0, 0, false); run(i, false, 0, 0, false); }
        run(0, true, 1, 0, false);
        for (unsigned i = 1; i <= count * 3; ++i) run(0, true, 0, i, false);
        run(0, true, 0, 0, true);
    }
    struct quest_camera_front_end *s = NULL;
    unsigned before = operations;
    for (unsigned i = 0; i < 21; ++i) {
        struct quest_camera_ife_setup c = configuration();
        switch (i) {
        case 0: c.hardware.valid = 0; break;
        case 1: c.hardware.id[0] = 1; break;
        case 2: c.hardware.id[0] = 0x100; break;
        case 3: c.hardware.id[0] = 0xc; break;
        case 4: c.hardware.id[1] = 4; break;
        case 5: c.hardware.paths[0][1] = 8; break;
        case 6: c.hardware.paths[1][0] = 8; break;
        case 7: c.hardware.paths[0][0] = 1; break;
        case 8: c.input.revision = 0x650100; break;
        case 9: c.input.rdi = 4; break;
        case 10: c.clocks.rdi = (rdi + 1) % 4; break;
        case 11: c.bandwidth.rdi = (rdi + 1) % 4; break;
        case 12: c.csid_clock_hz = 0; break;
        case 13: c.command_mmu = -1; break;
        case 14: c.command_mapping = QUEST_CAMERA_MEMORY_CPU; break;
        case 15: c.target.crm_fd = c.target.device_fd; break;
        case 16: c.target.device_fd = -1; break;
        case 17: c.target.session = 0; break;
        case 18: c.target.device = 0; break;
        case 19: c.input.width = 0; break;
        case 20: c.input.height = 4097; break;
        }
        assert(quest_camera_front_end_prepare_ife(&c, &s) == -EINVAL && !s && operations == before);
    }
    assert(quest_camera_front_end_prepare_ife(NULL, NULL) == -EINVAL);
    assert(quest_camera_front_end_prepare_phy(NULL, NULL, &s) == -EINVAL);
    assert(quest_camera_front_end_submit(NULL) == -EINVAL);
    assert(quest_camera_front_end_quiesced(NULL) == -EINVAL);
    assert(quest_camera_front_end_release(NULL) == -EINVAL);
    quest_camera_front_end_closed(NULL);
    printf("QUEST_CAMERA_FRONT_END_PASS cycles=%u faults=%u hardware=0\n", cycles, faults);
}
