#include "pico-camera-front-end.h"
#include "pico-camera-buffer.h"
#include "pico-camera-ife.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

struct pico_camera_buffer { struct pico_camera_buffer_view view; bool ready, flight, cpu; };
static struct pico_camera_buffer *buffers[3];
static unsigned next_buffer, live, calls, fail_at, configs, cycles, faults;
static bool ife, fail_ioctl;
static struct pico_camera_ife_setup expected;
static struct pico_camera_csiphy_config phy;
static uint64_t get(const void *data, unsigned offset, unsigned size)
{
    const uint8_t *p = (const uint8_t *)data + offset; uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static int memory_failure(void) { return ++calls == fail_at ? -EIO : 0; }
int pico_camera_buffer_create(int crm, int ion, const struct pico_camera_buffer_config *c,
                              struct pico_camera_buffer **out)
{
    assert(crm == 31 && ion == -1 && next_buffer < (ife ? 3U : 2U) && out && !*out);
    bool command = next_buffer < (ife ? 2U : 1U), mapped = ife && !next_buffer;
    assert(c->kind == (command ? PICO_CAMERA_BUFFER_COMMAND : PICO_CAMERA_BUFFER_PACKET));
    assert(c->mapping == (mapped ? expected.command_mapping : PICO_CAMERA_MEMORY_CPU));
    assert(c->mmu_count == (mapped ? 1U : 0U) && (!mapped || c->mmus[0] == 55));
    assert(c->cached && c->size == (mapped ? 8192U : 4096U));
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
    unsigned index = (unsigned)get(config, 16, 8) - 100;
    assert(index == (ife ? 2U : 1U));
    for (unsigned i = 0; i <= index; ++i) assert(buffers[i]->flight && !buffers[i]->cpu);
    const uint8_t *packet = buffers[index]->view.data;
    assert(get(packet, 0, 4) == (ife ? 0x0f000000U : 0x05000000U));
    assert(get(packet, 4, 4) == (ife ? 104U : 80U) && !get(packet, 8, 8));
    assert(get(packet, 28, 4) == index && !get(packet, 36, 4));
    unsigned direct_size = ife ? (expected.configure_throttle ? 40 : 28) : 32;
    assert(get(packet, 48, 4) == (ife ? 0U : UINT32_MAX));
    assert(get(packet, 52, 4) == (ife ? direct_size : 0));
    assert(get(packet, 56, 4) == 100 && !get(packet, 60, 4));
    assert(get(packet, 64, 4) == (ife ? 8192U : 4096U));
    assert(get(packet, 68, 4) == direct_size && get(packet, 72, 4) == 5);
    assert(get(packet, 76, 4) == (ife ? 3U : 0U));
    uint8_t direct[40] = {0};
    if (ife) {
        unsigned offset = expected.configure_throttle ? 12 : 0;
        if (offset) assert(pico_camera_ife_throttle_command(expected.utilization, direct, sizeof(direct)));
        struct pico_camera_ife_rdi rdi = {expected.hardware.id[0] >= 4, 0, expected.sample_pattern, 1};
        assert(pico_camera_ife_rdi_commands(&rdi, direct + offset, sizeof(direct) - offset));
        uint8_t generic[1228];
        assert(pico_camera_ife_hfr_blob(1, false, generic, 36));
        assert(pico_camera_ife_clock_blob(&expected.clocks, generic + 36, 60));
        assert(pico_camera_ife_bandwidth_blob(&expected.bandwidth, generic + 96, 1132));
        assert(get(packet, 80, 4) == 101 && !get(packet, 84, 4) && get(packet, 88, 4) == 4096);
        assert(get(packet, 92, 4) == sizeof(generic) && get(packet, 96, 4) == 9 && get(packet, 100, 4) == 12);
        assert(!memcmp(buffers[1]->view.data, generic, sizeof(generic)));
    } else assert(pico_camera_csiphy_configure(&phy, direct, sizeof(direct)));
    assert(!memcmp(buffers[0]->view.data, direct, direct_size));
    for (size_t i = direct_size; i < buffers[0]->view.size; ++i) assert(!((uint8_t *)buffers[0]->view.data)[i]);
    ++configs;
    if (fail_ioctl) { errno = EINTR; return -1; }
    return 0;
}
static struct pico_camera_ife_setup configuration(unsigned n)
{
    return (struct pico_camera_ife_setup){.target = {31,32,41,42},
        .hardware = {.id = {1U << (n % 7)}, .paths = {{8}}, .valid = 1},
        .command_mmu = 55, .command_mapping = n & 1 ? PICO_CAMERA_MEMORY_IO : PICO_CAMERA_MEMORY_SHARED,
        .sample_pattern = (n / 7) % 8, .clocks = {400000000,0,200000000,false},
        .bandwidth = {640000000,800000000,1000000000}, .configure_throttle = (n / 56) & 1,
        .utilization = (float)(n % 5) / 4};
}
static void run(bool isp, unsigned memory_error, bool io_error, unsigned n)
{
    assert(!live); ife = isp; calls = next_buffer = 0; fail_at = memory_error; fail_ioctl = io_error;
    expected = configuration(n); phy = (struct pico_camera_csiphy_config){0,0,1,0,12,400000000,0};
    struct pico_camera_ife_setup c = expected; struct pico_camera_csiphy_config p = phy;
    struct pico_camera_front_end *s = NULL;
    int result = isp ? pico_camera_front_end_prepare_ife(&c, &s) : pico_camera_front_end_prepare_phy(&c.target, &p, &s);
    assert(s);
    unsigned before = configs, count = isp ? 3 : 2;
    if (memory_error && memory_error <= count * 3) {
        assert(result == -EIO && pico_camera_front_end_submit(s) == -EINVAL);
    } else {
        assert(!result); memset(&c, 0xa5, sizeof(c)); memset(&p, 0xa5, sizeof(p));
        result = pico_camera_front_end_submit(s);
        assert(result == (io_error ? -EINTR : memory_error ? -EIO : 0));
        assert(pico_camera_front_end_release(&s) == -EBUSY);
        assert(pico_camera_front_end_submit(s) == -EINVAL);
        assert(configs == before + (memory_error ? 0 : 1));
        assert(!pico_camera_front_end_quiesced(s));
        assert(pico_camera_front_end_quiesced(s) == -EINVAL);
    }
    assert(!pico_camera_front_end_release(&s) && !s && !live);
    assert(!pico_camera_front_end_release(&s));
    if (memory_error || io_error) ++faults; else ++cycles;
}
int main(void)
{
    for (unsigned n = 0; n < 448; ++n) run(true,0,false,n);
    for (unsigned n = 0; n < 100; ++n) run(false,0,false,n);
    for (unsigned i = 0; i < 2; ++i) {
        for (unsigned fault = 1; fault <= (i ? 12U : 8U); ++fault) run(i,fault,false,100);
        run(i,0,true,100);
    }
    for (unsigned invalid = 0; invalid < 15; ++invalid) {
        struct pico_camera_ife_setup c = configuration(0); struct pico_camera_front_end *s = NULL;
        switch (invalid) {
        case 0: c.hardware.valid = 0; break; case 1: c.hardware.valid = 2; break;
        case 2: c.hardware.id[0] = 3; break; case 3: c.hardware.id[0] = 256; break;
        case 4: c.hardware.paths[0][0] = 16; break; case 5: c.hardware.paths[0][1] = 8; break;
        case 6: c.hardware.id[1] = 1; break; case 7: c.hardware.paths[4][2] = 8; break;
        case 8: c.command_mapping = PICO_CAMERA_MEMORY_CPU; break; case 9: c.command_mmu = -1; break;
        case 10: c.clocks.dual = true; break; case 11: c.bandwidth.mnoc_peak_bytes_per_second = 1; break;
        case 12: c.sample_pattern = 8; break; case 13: c.target.device = 0; break;
        case 14: c.configure_throttle = true; c.utilization = NAN; break;
        }
        unsigned before = calls; assert(pico_camera_front_end_prepare_ife(&c,&s) == -EINVAL && !s && before == calls);
    }
    assert(pico_camera_front_end_prepare_ife(NULL,NULL) == -EINVAL);
    assert(pico_camera_front_end_prepare_phy(NULL,NULL,NULL) == -EINVAL);
    assert(pico_camera_front_end_submit(NULL) == -EINVAL);
    assert(pico_camera_front_end_quiesced(NULL) == -EINVAL);
    assert(pico_camera_front_end_release(NULL) == -EINVAL);
    printf("PICO_CAMERA_FRONT_END_PASS cycles=%u faults=%u configurations=%u\n",cycles,faults,configs);
}
