#include "pico-camera-probe-setup.h"
#include "pico-camera-buffer.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

struct pico_camera_buffer { struct pico_camera_buffer_view view; bool ready, cpu, flight; };
static struct pico_camera_buffer *buffers[3];
static unsigned memory_calls, fail_memory, view_calls, fail_view, io_calls, next_buffer, live;
static unsigned cycles, faults, probes;
static int io_error;
static bool fail_complete, fail_release;
static uint8_t identity[28], power[672];
static size_t power_size;
static uint64_t get(const void *data, unsigned offset, unsigned size)
{
    const uint8_t *p = (const uint8_t *)data + offset; uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static int memory_failure(void) { return ++memory_calls == fail_memory ? -EIO : 0; }
int pico_camera_buffer_create(int crm, int ion, const struct pico_camera_buffer_config *c,
                              struct pico_camera_buffer **out)
{
    assert(crm == 31 && ion == -1 && next_buffer < 3 && out && !*out);
    assert(c->mapping == PICO_CAMERA_MEMORY_CPU && !c->mmu_count && c->cached && c->size == 4096);
    assert(c->kind == (next_buffer == 2 ? PICO_CAMERA_BUFFER_PACKET : PICO_CAMERA_BUFFER_COMMAND));
    struct pico_camera_buffer *b = calloc(1, sizeof(*b)); assert(b);
    b->view = (struct pico_camera_buffer_view){100 + (int)next_buffer, 200 + (int)next_buffer, calloc(1, c->size), c->size};
    assert(b->view.data); buffers[next_buffer++] = b; *out = b; ++live;
    int result = memory_failure(); b->ready = !result; return result;
}
bool pico_camera_buffer_view(const struct pico_camera_buffer *b, struct pico_camera_buffer_view *v)
{ if (++view_calls == fail_view || !b || !b->ready) return false; *v = b->view; return true; }
int pico_camera_buffer_cpu_begin(struct pico_camera_buffer *b, enum ion_buffer_access access)
{
    assert(b && b->ready && !b->flight && !b->cpu && access == ION_BUFFER_WRITE);
    int result = memory_failure(); if (!result) b->cpu = true; return result;
}
int pico_camera_buffer_cpu_end(struct pico_camera_buffer *b)
{ assert(b && b->cpu); b->cpu = false; return memory_failure(); }
int pico_camera_buffer_submit(struct pico_camera_buffer *b)
{ assert(b && b->ready && !b->cpu && !b->flight); int result = memory_failure(); if (!result) b->flight = true; return result; }
int pico_camera_buffer_complete(struct pico_camera_buffer *b)
{
    assert(b && b->flight && !b->cpu);
    if (fail_complete) return -EIO;
    b->flight = false; return 0;
}
int pico_camera_buffer_release(struct pico_camera_buffer **out)
{
    if (!*out) return 0;
    struct pico_camera_buffer *b = *out;
    assert(!b->flight && !b->cpu && live);
    if (fail_release) return -EIO;
    --live; buffers[b->view.memory_handle - 100] = NULL;
    memset(b->view.data, 0xa5, b->view.size); free(b->view.data); free(b); *out = NULL; return 0;
}
#ifdef PICO_PROBE_KERNEL
void pico_probe_kernel_check(const void *packet, const void *identity, const void *power, size_t power_size);
#endif
int ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 32 && request == 0xc01856c0UL);
    va_list args; va_start(args, request); const void *control = va_arg(args, const void *); va_end(args);
    assert(get(control, 0, 4) == 0x10a && get(control, 4, 4) == 8 && get(control, 8, 4) == 2);
    assert(!get(control, 12, 4) && get(control, 16, 8) == 102);
    for (unsigned i = 0; i < 3; ++i) assert(buffers[i] && buffers[i]->flight && !buffers[i]->cpu);
    const void *packet = buffers[2]->view.data;
    assert(get(packet, 0, 4) == 0x1000003 && get(packet, 4, 4) == 104);
    assert(!get(packet, 8, 8) && !get(packet, 24, 4) && get(packet, 28, 4) == 2);
    assert(get(packet, 32, 4) == 48 && !get(packet, 36, 4) && get(packet, 40, 4) == 48);
    assert(get(packet, 48, 4) == UINT32_MAX && !get(packet, 52, 4));
    for (unsigned i = 0; i < 2; ++i) {
        unsigned off = 56 + i * 24;
        assert(get(packet, off, 4) == 100 + i && !get(packet, off + 4, 4));
        assert(get(packet, off + 8, 4) == 4096 && get(packet, off + 12, 4) == (i ? power_size : 28));
        assert(get(packet, off + 16, 4) == (i ? 7 : 10) && !get(packet, off + 20, 4));
        assert(!memcmp(buffers[i]->view.data, i ? power : identity, i ? power_size : 28));
        const uint8_t *bytes = buffers[i]->view.data;
        for (size_t j = i ? power_size : 28; j < 4096; ++j) assert(!bytes[j]);
    }
#ifdef PICO_PROBE_KERNEL
    pico_probe_kernel_check(packet, buffers[0]->view.data, buffers[1]->view.data, power_size);
#endif
    ++probes; ++io_calls;
    if (io_error < 0) { errno = -io_error; return -1; }
    return io_error;
}
static struct pico_camera_probe_setup_config config(struct pico_camera_sensor_power_step *up,
                                                    struct pico_camera_sensor_power_step *down, size_t count)
{
    return (struct pico_camera_probe_setup_config){.crm_fd = 31, .sensor_fd = 32,
        .bus = {2, 1, 1, true, 0xe0}, .identity = {0x300a, 0x77, 0xff, 4, 1},
        .power_up = up, .power_down = down, .power_up_count = count, .power_down_count = count};
}
static void run(unsigned count, unsigned memory_error, unsigned view_error, int error, bool completion, bool release)
{
    assert(!live); memory_calls = view_calls = io_calls = next_buffer = 0;
    fail_memory = memory_error; fail_view = view_error; io_error = error; fail_complete = fail_release = false;
    struct pico_camera_sensor_power_step up[12], down[12];
    for (unsigned i = 0; i < count; ++i) {
        up[i] = (struct pico_camera_sensor_power_step){i, 100 + i, i % 3 + 1};
        down[i] = (struct pico_camera_sensor_power_step){count - i - 1, 0, 32767};
    }
    struct pico_camera_probe_setup_config c = config(up, down, count);
    assert(pico_camera_sensor_probe(&c.bus, &c.identity, identity, sizeof(identity)) == sizeof(identity));
    size_t first = pico_camera_sensor_power(true, up, count, power, sizeof(power)); assert(first);
    size_t second = pico_camera_sensor_power(false, down, count, power + first, sizeof(power) - first); assert(second);
    power_size = first + second;
    struct pico_camera_probe_setup *s = NULL;
    int result = pico_camera_probe_setup_prepare(&c, &s); assert(s);
    if ((memory_error && memory_error <= 9) || (view_error && view_error <= 3)) {
        assert(result == (view_error ? -EINVAL : -EIO) && !io_calls);
        assert(pico_camera_probe_setup_submit(s) == -EINVAL);
        assert(pico_camera_probe_setup_quiesced(s) == -EINVAL);
    } else {
        assert(!result && !io_calls && live == 3);
        assert(pico_camera_probe_setup_prepare(&c, &s) == -EINVAL);
        assert(pico_camera_probe_setup_quiesced(s) == -EINVAL);
        memset(up, 0xa5, sizeof(up)); memset(down, 0xa5, sizeof(down)); memset(&c, 0xa5, sizeof(c));
        result = pico_camera_probe_setup_submit(s);
        assert(result == (view_error ? -EINVAL : memory_error ? -EIO : error > 0 ? -EPROTO : error));
        unsigned calls = io_calls;
        assert(pico_camera_probe_setup_submit(s) == -EINVAL && io_calls == calls);
        assert(pico_camera_probe_setup_release(&s) == -EBUSY);
        if (completion) {
            fail_complete = true; assert(pico_camera_probe_setup_quiesced(s) == -EIO);
            assert(pico_camera_probe_setup_release(&s) == -EBUSY); fail_complete = false;
        }
        assert(!pico_camera_probe_setup_quiesced(s));
        assert(pico_camera_probe_setup_quiesced(s) == -EINVAL);
        if (release) {
            fail_release = true; assert(pico_camera_probe_setup_release(&s) == -EIO && s && live == 3);
            assert(pico_camera_probe_setup_submit(s) == -EINVAL); fail_release = false;
        }
        if (!memory_error && !view_error) assert(io_calls == 1);
        else assert(!io_calls);
    }
    assert(!pico_camera_probe_setup_release(&s) && !s && !live);
    assert(!pico_camera_probe_setup_release(&s));
    if (memory_error || view_error || error || completion || release) ++faults; else ++cycles;
}
int main(void)
{
    for (unsigned i = 0; i < 240; ++i) run(i % 12 + 1, 0, 0, 0, false, false);
    for (unsigned i = 1; i <= 12; ++i) run(12, i, 0, 0, false, false);
    for (unsigned i = 1; i <= 4; ++i) run(12, 0, i, 0, false, false);
    run(12, 0, 0, -EINTR, false, false); run(12, 0, 0, 1, false, false);
    run(12, 0, 0, 0, true, false); run(12, 0, 0, 0, false, true);
    assert(pico_camera_probe_setup_prepare(NULL, NULL) == -EINVAL);
    assert(pico_camera_probe_setup_submit(NULL) == -EINVAL);
    assert(pico_camera_probe_setup_quiesced(NULL) == -EINVAL);
    assert(pico_camera_probe_setup_release(NULL) == -EINVAL);
    struct pico_camera_sensor_power_step step = {0, 24000000, 1};
    struct pico_camera_probe_setup_config c = config(&step, &step, 1);
    struct pico_camera_probe_setup *s = NULL; unsigned calls = memory_calls;
#define BAD(field, value) do { struct pico_camera_probe_setup_config bad = c; bad.field = value; \
    assert(pico_camera_probe_setup_prepare(&bad, &s) == -EINVAL && !s && calls == memory_calls); } while (0)
    BAD(crm_fd, -1); BAD(sensor_fd, -1); BAD(bus.slave_address, 1); BAD(bus.include_slave, false);
    BAD(identity.pipeline_delay, 4); BAD(power_up, NULL); BAD(power_down, NULL);
    BAD(power_up_count, 0); BAD(power_down_count, 13);
    printf("PICO_CAMERA_PROBE_SETUP_PASS cycles=%u faults=%u probes=%u\n", cycles, faults, probes);
}
