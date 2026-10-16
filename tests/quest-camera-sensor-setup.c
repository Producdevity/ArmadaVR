#define _GNU_SOURCE
#include "quest-camera-sensor-setup.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef QUEST_CAMERA_SETUP_INSTALLED
#define quest_test_ioctl ioctl
#define quest_test_fcntl fcntl
#define quest_test_mmap mmap
#define quest_test_munmap munmap
#define quest_test_close close
#endif

static struct { unsigned char data[4096]; bool fd, handle, mapped; } buffers[10];
static unsigned next_buffer, operations, fail_at, submitted, variant, cycles, faults;
static unsigned active[5], active_count, values[5], phase;
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
    assert(i % 2 && submitted < active_count && i == submitted * 2 + 1);
    unsigned stage = active[submitted];
    const unsigned opcodes[] = {0x1000002, 0x1000004, 0x1000004, 0x1000000, 0x1000005};
    const unsigned char *packet = buffers[i].data, *command = buffers[i - 1].data;
    assert(buffers[i].mapped && buffers[i - 1].mapped && buffers[i].handle && buffers[i - 1].handle);
    assert(get(packet, 0, 4) == opcodes[stage] && get(packet, 4, 4) == 80 && !get(packet, 8, 8));
    assert(get(packet, 28, 4) == 1 && !get(packet, 36, 4) && get(packet, 48, 4) == UINT32_MAX);
    assert(get(packet, 56, 4) == handle(i - 1) && !get(packet, 60, 4) && get(packet, 64, 4) == 4096);
    unsigned length = stage ? 32 : 24;
    assert(get(packet, 68, 4) == length && get(packet, 72, 4) == 7 && !get(packet, 76, 4));
    const unsigned char bus[] = {0xe0, 0, 0, 0, 1, 4, 0, 0, 1, 0, 0, 0, 1, 5, 1, 2};
    assert(!memcmp(command, bus, sizeof(bus)) && get(command, 16, 4) == 0x100 + stage && get(command, 20, 4) == values[stage]);
    if (stage) {
        assert(get(command, 24, 2) == (stage == 1 ? 1000 : stage));
        assert(!get(command, 26, 2) && command[28] == (stage == 1 ? 2 : 3) && command[29] == 9 && !get(command, 30, 2));
    }
    for (unsigned n = length; n < 4096; ++n) assert(!command[n]);
    for (unsigned n = 80; n < 4096; ++n) assert(!packet[n]);
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
        assert(!phase && fd == 31 && get(p, 4, 4) == 104 && next_buffer < 10);
        assert(get(data, 0, 8) == 4096 && get(data, 8, 8) == 4096 && !get(data, 80, 4));
        unsigned i = next_buffer++;
        unsigned flags = 8 | 16 | 8192 | 1024 | (i % 2 ? 512 : 64);
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
    assert(!phase && !address && size == 4096 && prot == (PROT_READ | PROT_WRITE) && flags == MAP_SHARED && !offset);
    assert(fd >= 100 && (unsigned)fd < 100 + next_buffer && buffers[fd - 100].fd);
    if (failure()) { errno = EINTR; return MAP_FAILED; }
    buffers[fd - 100].mapped = true; memset(buffers[fd - 100].data, 0xa5, 4096);
    return buffers[fd - 100].data;
}
int quest_test_munmap(void *address, size_t size)
{
    assert(phase == 2 && quiesced && size == 4096);
    unsigned i = 0; while (i < next_buffer && address != buffers[i].data) ++i;
    assert(i < next_buffer && buffers[i].mapped && !buffers[i].handle);
    if (failure()) return failed();
    buffers[i].mapped = false; return 0;
}
int quest_test_close(int fd)
{
    assert(phase == 2 && quiesced && fd >= 100 && (unsigned)fd < 100 + next_buffer);
    unsigned i = fd - 100; assert(buffers[i].fd && !buffers[i].mapped && !buffers[i].handle);
    buffers[i].fd = false; return failure() ? failed() : 0;
}
static void run(unsigned prepare_error, bool copy, unsigned configure_error, unsigned cleanup_error)
{
    for (unsigned i = 0; i < 10; ++i) assert(!buffers[i].fd && !buffers[i].handle && !buffers[i].mapped);
    next_buffer = operations = submitted = phase = active_count = 0;
    fail_at = prepare_error; partial = copy; lost = quiesced = false;
    struct quest_camera_sensor_write writes[5]; struct quest_camera_sensor_table tables[5];
    for (unsigned i = 0; i < 5; ++i) {
        values[i] = (cycles + i) & 255;
        writes[i] = (struct quest_camera_sensor_write){0x100 + i, values[i], i * 1000};
        tables[i] = (struct quest_camera_sensor_table){{2, 1, 1, true, 0xe0}, writes + i, 1};
        if ((i == 1 && !(variant & 1)) || (i == 2 && !(variant & 2))) tables[i].count = 0;
        else active[active_count++] = i;
    }
    struct quest_camera_sensor_setup_config c = {31, 32, 41, 42, tables[0], tables[1], tables[2], tables[3], tables[4]};
    struct quest_camera_sensor_setup *s = NULL;
    int result = quest_camera_sensor_setup_prepare(&c, &s);
    assert(s && result == (prepare_error ? -EINTR : 0));
    unsigned before = operations;
    if (prepare_error) assert(quest_camera_sensor_setup_submit(s) == -EINVAL && operations == before);
    else {
        assert(operations == active_count * 10);
        memset(writes, 0xa5, sizeof(writes)); memset(&c, 0xa5, sizeof(c));
        phase = 1; operations = 0; fail_at = configure_error;
        result = quest_camera_sensor_setup_submit(s);
        assert(result == (configure_error ? -EINTR : 0));
        assert(submitted == (configure_error ? configure_error : active_count));
        before = operations;
        assert(quest_camera_sensor_setup_submit(s) == -EINVAL && operations == before);
    }
    before = operations;
    assert(quest_camera_sensor_setup_release(&s) == -EBUSY && before == operations);
    assert(!quest_camera_sensor_setup_quiesced(s) && quest_camera_sensor_setup_quiesced(s) == -EINVAL);
    quiesced = true; phase = 2; operations = 0; fail_at = cleanup_error;
    result = quest_camera_sensor_setup_release(&s);
    if (lost || cleanup_error) {
        assert(s && result == (lost ? -EIO : -EINTR)); before = operations;
        assert(quest_camera_sensor_setup_release(&s) == result && operations == before);
        struct quest_camera_allocation_view view;
        for (unsigned i = 0; i < 5; ++i) for (unsigned packet = 0; packet < 2; ++packet)
            if (quest_camera_sensor_setup_resources(s, i, packet, &view)) {
                assert(view.size == 4096);
                if (view.mapped) assert(view.data != NULL && view.fd >= 100);
            }
        for (unsigned i = 0; i < next_buffer; ++i) buffers[i].fd = buffers[i].handle = buffers[i].mapped = false;
        quest_camera_sensor_setup_closed(&s); assert(!s && before == operations);
    } else {
        assert(!result && !s);
        for (unsigned i = 0; i < next_buffer; ++i) assert(!buffers[i].fd && !buffers[i].handle && !buffers[i].mapped);
    }
    assert(!quest_camera_sensor_setup_release(&s));
    if (prepare_error || configure_error || cleanup_error) ++faults; else ++cycles;
}
int main(void)
{
    for (variant = 0; variant < 4; ++variant) {
        unsigned stages = 3 + (variant & 1) + ((variant >> 1) & 1);
        for (unsigned i = 0; i < 200; ++i) run(0, true, 0, 0);
        for (unsigned i = 1; i <= stages * 10; ++i) { run(i, true, 0, 0); run(i, false, 0, 0); }
        for (unsigned i = 1; i <= stages; ++i) run(0, true, i, 0);
        for (unsigned i = 1; i <= stages * 6; ++i) run(0, true, 0, i);
    }
    assert(quest_camera_sensor_setup_prepare(NULL, NULL) == -EINVAL);
    assert(quest_camera_sensor_setup_submit(NULL) == -EINVAL);
    assert(quest_camera_sensor_setup_quiesced(NULL) == -EINVAL);
    assert(quest_camera_sensor_setup_release(NULL) == -EINVAL);
    quest_camera_sensor_setup_closed(NULL);
    printf("QUEST_CAMERA_SENSOR_SETUP_PASS cycles=%u faults=%u hardware=0\n", cycles, faults);
}
