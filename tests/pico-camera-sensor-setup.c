#include "pico-camera-sensor-setup.h"
#include "pico-camera-buffer.h"
#include "pico-camera-isp.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

struct pico_camera_buffer { struct pico_camera_buffer_view view; bool ready, flight, cpu; };
static struct pico_camera_buffer *buffers[10];
static unsigned memory_calls, fail_memory_at, io_calls, fail_io_at, next_buffer, live;
static unsigned stages, submitted, configurations, cycles, faults;
static bool synchronize;
static uint32_t expected_values[5];
static uint64_t get(const void *data, unsigned offset, unsigned size)
{
    const uint8_t *p = (const uint8_t *)data + offset; uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static int memory_failure(void) { return ++memory_calls == fail_memory_at ? -EIO : 0; }
int pico_camera_buffer_create(int crm, int ion, const struct pico_camera_buffer_config *c,
                              struct pico_camera_buffer **out)
{
    assert(crm == 31 && ion == -1 && next_buffer < 10 && out && !*out);
    assert(c->mapping == PICO_CAMERA_MEMORY_CPU && !c->mmu_count && c->cached && c->size == 4096);
    assert(c->kind == (next_buffer % 2 ? PICO_CAMERA_BUFFER_PACKET : PICO_CAMERA_BUFFER_COMMAND));
    struct pico_camera_buffer *b = calloc(1, sizeof(*b)); assert(b);
    b->view = (struct pico_camera_buffer_view){100 + (int)next_buffer, 200 + (int)next_buffer, calloc(1, c->size), c->size};
    assert(b->view.data); buffers[next_buffer++] = b; *out = b; ++live;
    int result = memory_failure(); b->ready = !result; return result;
}
bool pico_camera_buffer_view(const struct pico_camera_buffer *b, struct pico_camera_buffer_view *v)
{ if (!b || !b->ready) return false; *v = b->view; return true; }
int pico_camera_buffer_cpu_begin(struct pico_camera_buffer *b, enum ion_buffer_access access)
{
    assert(b && b->ready && !b->flight && !b->cpu && access == ION_BUFFER_WRITE);
    int result = memory_failure(); if (!result) b->cpu = true; return result;
}
int pico_camera_buffer_cpu_end(struct pico_camera_buffer *b)
{ assert(b && b->cpu); b->cpu = false; return memory_failure(); }
int pico_camera_buffer_submit(struct pico_camera_buffer *b)
{ assert(b && b->ready && !b->flight && !b->cpu); int result = memory_failure(); if (!result) b->flight = true; return result; }
int pico_camera_buffer_complete(struct pico_camera_buffer *b)
{ assert(b && b->flight && !b->cpu); b->flight = false; return 0; }
int pico_camera_buffer_release(struct pico_camera_buffer **out)
{
    if (!*out) return 0;
    struct pico_camera_buffer *b = *out;
    assert(!b->flight && !b->cpu && live); --live; buffers[b->view.memory_handle - 100] = NULL;
    memset(b->view.data, 0xa5, b->view.size); free(b->view.data); free(b); *out = NULL; return 0;
}
int ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 32 && request == 0xc01856c0UL);
    va_list args; va_start(args, request); const void *control = va_arg(args, const void *); va_end(args);
    assert(get(control, 0, 4) == 0x105 && get(control, 4, 4) == 24);
    const void *config = (void *)(uintptr_t)get(control, 16, 8);
    assert(get(config, 0, 4) == 41 && get(config, 4, 4) == 42 && !get(config, 8, 8));
    unsigned packet_id = (unsigned)get(config, 16, 8) - 100;
    assert(packet_id < 10 && packet_id % 2 && buffers[packet_id]->flight && buffers[packet_id - 1]->flight);
    const void *packet = buffers[packet_id]->view.data;
    unsigned stage = submitted; if (!synchronize && stage >= 2) ++stage;
    const unsigned expected_opcode[] = {0x1000002, 0x1000004, 0x1000004, 0x1000000, 0x1000005};
    assert(get(packet, 0, 4) == expected_opcode[stage] && get(packet, 4, 4) == 80);
    assert(!get(packet, 8, 8) && get(packet, 28, 4) == 1 && !get(packet, 36, 4));
    assert(get(packet, 48, 4) == UINT32_MAX && !get(packet, 52, 4));
    assert(get(packet, 56, 4) == 100 + packet_id - 1 && !get(packet, 60, 4));
    assert(get(packet, 64, 4) == 4096 && get(packet, 72, 4) == 7 && !get(packet, 76, 4));
    uint8_t expected[128];
    struct pico_camera_sensor_bus bus = {2, 1, 1, true, 0xe0};
    struct pico_camera_sensor_write write = {0x100 + stage, expected_values[stage], stage * 1000};
    size_t size = pico_camera_sensor_writes(&bus, &write, 1, expected, sizeof(expected));
    assert(size && get(packet, 68, 4) == size);
    assert(!memcmp(buffers[packet_id - 1]->view.data, expected, size));
    ++submitted; ++configurations;
    if (++io_calls == fail_io_at) { errno = EINTR; return -1; }
    return 0;
}
static void run(bool sync, unsigned memory_error, unsigned io_error)
{
    assert(!live); synchronize = sync; stages = sync ? 5 : 4;
    memory_calls = io_calls = next_buffer = submitted = 0;
    fail_memory_at = memory_error; fail_io_at = io_error;
    struct pico_camera_sensor_write writes[5];
    struct pico_camera_sensor_table tables[5];
    for (unsigned i = 0; i < 5; ++i) {
        expected_values[i] = (cycles + i) & 255;
        writes[i] = (struct pico_camera_sensor_write){0x100 + i, expected_values[i], i * 1000};
        tables[i] = (struct pico_camera_sensor_table){{2, 1, 1, true, 0xe0}, writes + i, 1};
    }
    if (!sync) tables[2].count = 0;
    struct pico_camera_sensor_setup_config c = {31, 32, 41, 42, tables[0], tables[1], tables[2], tables[3], tables[4]};
    struct pico_camera_sensor_setup *s = NULL;
    int result = pico_camera_sensor_setup_prepare(&c, &s);
    assert(s);
    if (memory_error && memory_error <= stages * 6) {
        assert(result == -EIO && !io_calls);
        assert(pico_camera_sensor_setup_submit(s) == -EINVAL);
    } else {
        assert(!result && !io_calls);
        memset(writes, 0xa5, sizeof(writes)); memset(&c, 0xa5, sizeof(c));
        result = pico_camera_sensor_setup_submit(s);
        assert(result == (io_error ? -EINTR : memory_error ? -EIO : 0));
        assert(pico_camera_sensor_setup_release(&s) == -EBUSY);
        unsigned calls = io_calls;
        assert(pico_camera_sensor_setup_submit(s) == -EINVAL && calls == io_calls);
        assert(!pico_camera_sensor_setup_quiesced(s));
        assert(pico_camera_sensor_setup_quiesced(s) == -EINVAL);
        if (!memory_error && !io_error) assert(submitted == stages);
    }
    assert(!pico_camera_sensor_setup_release(&s) && !s && !live);
    assert(!pico_camera_sensor_setup_release(&s));
    if (memory_error || io_error) ++faults; else ++cycles;
}
int main(void)
{
    for (unsigned i = 0; i < 400; ++i) run(i % 2, 0, 0);
    for (unsigned sync = 0; sync < 2; ++sync) {
        unsigned count = sync ? 5 : 4;
        for (unsigned i = 1; i <= count * 8; ++i) run(sync, i, 0);
        for (unsigned i = 1; i <= count; ++i) run(sync, 0, i);
    }
    assert(pico_camera_sensor_setup_prepare(NULL, NULL) == -EINVAL);
    assert(pico_camera_sensor_setup_submit(NULL) == -EINVAL);
    assert(pico_camera_sensor_setup_quiesced(NULL) == -EINVAL);
    assert(pico_camera_sensor_setup_release(NULL) == -EINVAL);
    printf("PICO_CAMERA_SENSOR_SETUP_PASS cycles=%u faults=%u configurations=%u\n", cycles, faults, configurations);
}
