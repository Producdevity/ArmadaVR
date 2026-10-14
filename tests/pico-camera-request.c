#include "pico-camera-request.h"
#include "pico-camera-stream.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

struct pico_camera_buffer {
    struct pico_camera_buffer_view view;
    bool ready, flight, cpu;
};
static struct pico_camera_buffer *buffers[64];
static unsigned next_buffer, live_buffers, io_calls, fail_io_at, memory_calls, fail_memory_at;
static unsigned sensor_configs, ife_configs, schedules, fence_destroys, frames, releases;
static unsigned order, fences;
static uint64_t expected_request, expected_generation;
static struct pico_camera_request *retained;
static uint64_t get(const void *data, unsigned offset, unsigned size)
{
    const uint8_t *p = (const uint8_t *)data + offset; uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void set(void *data, unsigned offset, uint64_t value, unsigned size)
{
    uint8_t *p = (uint8_t *)data + offset;
    for (unsigned i = 0; i < size; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
static int memory_failure(void) { return ++memory_calls == fail_memory_at ? -EIO : 0; }
int pico_camera_buffer_create(int crm, int ion, const struct pico_camera_buffer_config *c,
                              struct pico_camera_buffer **out)
{
    assert(crm == 31 && ion == 32 && out && !*out && next_buffer < 64);
    struct pico_camera_buffer *b = calloc(1, sizeof(*b)); assert(b);
    *out = b; unsigned id = next_buffer++; buffers[id] = b; ++live_buffers;
    b->view = (struct pico_camera_buffer_view){100 + (int)id, 200 + (int)id, calloc(1, c->size), c->size};
    assert(b->view.data);
    switch (id % 4) {
    case 0: assert(c->kind == PICO_CAMERA_BUFFER_PIXELS && c->mapping == PICO_CAMERA_MEMORY_IO);
        assert(c->mmu_count == 1 && c->mmus[0] == 71 && c->ion_heap == 25 && c->size == 8192); break;
    case 1: case 2: assert(c->kind == PICO_CAMERA_BUFFER_PACKET && c->mapping == PICO_CAMERA_MEMORY_CPU);
        assert(!c->mmu_count && c->size == 4096); break;
    case 3: assert(c->kind == PICO_CAMERA_BUFFER_COMMAND && c->mapping == PICO_CAMERA_MEMORY_SHARED);
        assert(c->mmu_count == 1 && c->mmus[0] == 72 && c->size == 4096); break;
    }
    int result = memory_failure(); b->ready = !result; return result;
}
bool pico_camera_buffer_view(const struct pico_camera_buffer *b, struct pico_camera_buffer_view *v)
{ if (!b || !b->ready) return false; *v = b->view; return true; }
int pico_camera_buffer_cpu_begin(struct pico_camera_buffer *b, enum ion_buffer_access access)
{
    assert(b && b->ready && !b->flight && !b->cpu && (access == ION_BUFFER_READ || access == ION_BUFFER_WRITE));
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
    if (b->flight || b->cpu) return -EBUSY;
    assert(live_buffers); --live_buffers; buffers[b->view.memory_handle - 100] = NULL;
    memset(b->view.data, 0xa5, b->view.size); free(b->view.data); free(b); *out = NULL; return 0;
}
int ioctl(int fd, unsigned long request, ...)
{
    assert(request == 0xc01856c0UL);
    va_list args; va_start(args, request); const void *control = va_arg(args, const void *); va_end(args);
    unsigned opcode = (unsigned)get(control, 0, 4), size = (unsigned)get(control, 4, 4);
    void *data = (void *)(uintptr_t)get(control, 16, 8);
    if (++io_calls == fail_io_at) { errno = EINTR; return -1; }
    if (fd == 33) {
        if (opcode == 0) {
            assert(size == 68 && !strcmp(data, "pico-camera"));
            set(data, 64, 50 + ++fences, 4);
        } else if (opcode == 4) {
            assert(size == 24 && get(data, 8, 8) == expected_generation && get(data, 16, 8) == expected_request);
        } else { assert(opcode == 1 && size == 68); ++fence_destroys; }
    } else if (fd == 31) {
        assert(opcode == 0x10f && size == 32 && !order);
        assert(get(data, 0, 4) == 11 && get(data, 4, 4) == 21);
        assert(!get(data, 8, 4) && get(data, 12, 4) == 1 && get(data, 24, 8) == expected_request);
        ++schedules; order = 1;
    } else {
        assert((fd == 34 || fd == 35) && opcode == 0x105 && size == 24);
        assert(get(data, 0, 4) == 11 && get(data, 4, 4) == (unsigned)(fd == 34 ? 41 : 42));
        assert(!get(data, 8, 8)); unsigned id = (unsigned)get(data, 16, 8) - 100;
        assert(id < 64 && buffers[id] && buffers[id]->flight);
        const uint8_t *p = buffers[id]->view.data;
        assert(get(p, 8, 8) == expected_request);
        if (fd == 34) {
            assert(order == 1 && get(p, 0, 4) == 0x0100007f && get(p, 4, 4) == 56 && !get(p, 28, 4));
            ++sensor_configs; order = 2;
        } else {
            assert(order == 2 && get(p, 0, 4) == 0x0f000001 && get(p, 4, 4) == 336);
            assert(get(p, 28, 4) == 1 && get(p, 32, 4) == 24 && get(p, 36, 4) == 1);
            assert(!get(p, 48, 4) && !get(p, 52, 4));
            assert(get(p, 56, 4) == id + 101 && get(p, 64, 4) == 4096 && !get(p, 68, 4));
            assert(get(p, 72, 4) == 5 && get(p, 76, 4) == 3);
            assert(get(p, 80, 4) == id + 98 && get(p, 104, 4) == 65 && get(p, 108, 4) == 67);
            assert(get(p, 112, 4) == 80 && get(p, 116, 4) == 67 && get(p, 272, 4) > 50);
            ++ife_configs; order = 3;
        }
    }
    return 0;
}
static struct pico_camera_request_config config(void)
{
    return (struct pico_camera_request_config){.crm_fd = 31, .sync_fd = 33, .ion_fd = 32,
        .sensor_fd = 34, .ife_fd = 35, .session = 11, .link = 21, .sensor = 41, .ife = 42,
        .device_mmu = 71, .command_mmu = 72, .command_mapping = PICO_CAMERA_MEMORY_SHARED,
        .ion_heap = 25, .width = 65, .height = 67, .stride = 80, .camera_id = 7,
        .generation = expected_generation, .request_id = expected_request, .synchronized = true};
}
static void reset(void)
{
    assert(!live_buffers); next_buffer = 0; io_calls = memory_calls = fail_io_at = fail_memory_at = order = 0;
    expected_generation = 17; expected_request = UINT64_C(0x100000001);
}
static bool cpu(void *opaque, void *token, bool end)
{
    (void)opaque;
    if (!end && pico_camera_request_complete(token)) return false;
    return !pico_camera_request_cpu_sync(token, end);
}
static bool frame(void *opaque, uint64_t generation, const struct pico_camera_capture_frame *f, int64_t now)
{
    (void)opaque; assert(generation == 17 && now > 0 && f->buffer.pixels[0] == 0x5a); ++frames; return true;
}
static bool release(void *opaque, void *token)
{ (void)opaque; struct pico_camera_request *r = token; bool ok = !pico_camera_request_release(&r); if(ok) ++releases; return ok; }
static void invalid(void *opaque, uint64_t generation) { (void)opaque; assert(generation == 17); }
int main(void)
{
    for (unsigned cycle = 0; cycle < 300; ++cycle) {
        reset(); struct pico_camera_request_config c = config(); struct pico_camera_request *r = NULL;
        assert(!pico_camera_request_prepare(&c, &r));
        struct pico_camera_capture_buffer b; assert(pico_camera_request_capture(r, &b));
        struct pico_camera_stream_config sc = {.capture = {.generation = 17, .camera_ids = {7, 8}, .camera_count = 2, .timeout_ns = 1000},
            .session = 11, .links = {21, 22}, .first_request_id = expected_request};
        struct pico_camera_capture_ops ops = {cpu, frame, release, invalid}; struct pico_camera_stream *stream = NULL;
        assert(pico_camera_stream_create(&sc, &ops, NULL, &stream));
        assert(pico_camera_stream_reserve(stream, &b, -10, 100));
        assert(!pico_camera_request_submit(r) && order == 3);
        assert(pico_camera_request_submit(r) == -EINVAL);
        assert(pico_camera_request_release(&r) == -EBUSY);
        memset(buffers[0]->view.data, 0x5a, buffers[0]->view.size);
        struct pico_camera_event fence = {.type = 0x08000000}, sof = {.type = 0x08000000};
        set(fence.data, 0, b.fence, 4); set(fence.data, 4, 2, 4); set(fence.data, 8, 17, 8); set(fence.data, 16, expected_request, 8);
        set(sof.data, 0, 11, 4); set(sof.data, 32, 21, 4); set(sof.data, 16, 1, 8); set(sof.data, 8, expected_request, 8); set(sof.data, 24, 200, 8);
        assert(pico_camera_stream_event(stream, PICO_CAMERA_CRM_EVENTS, &sof, 200) == PICO_CAMERA_CAPTURE_PENDING);
        assert(pico_camera_stream_event(stream, PICO_CAMERA_SYNC_EVENTS, &fence, 200) == PICO_CAMERA_CAPTURE_DELIVERED);
        r = NULL; assert(!live_buffers); assert(pico_camera_stream_destroy(&stream));
    }
    for (unsigned fail = 1; fail <= 10; ++fail) {
        reset(); fail_memory_at = fail; struct pico_camera_request_config c = config(); struct pico_camera_request *r = NULL;
        assert(pico_camera_request_prepare(&c, &r) == -EIO && r);
        assert(pico_camera_request_submit(r) == -EINVAL); fail_memory_at = 0;
        assert(!pico_camera_request_release(&r) && !r && !live_buffers);
    }
    for (unsigned fail = 1; fail <= 5; ++fail) {
        reset(); fail_io_at = fail; struct pico_camera_request_config c = config(); struct pico_camera_request *r = NULL;
        int result = pico_camera_request_prepare(&c, &r);
        if (fail <= 2) assert(result == -EINTR);
        else {
            assert(!result && pico_camera_request_submit(r) == -EINTR);
            assert(pico_camera_request_release(&r) == -EBUSY && pico_camera_request_submit(r) == -EINVAL);
            assert(!pico_camera_request_complete(r));
        }
        fail_io_at = 0; assert(!pico_camera_request_release(&r) && !live_buffers);
    }
    for (unsigned fail = 11; fail <= 14; ++fail) {
        reset(); struct pico_camera_request_config c = config(); struct pico_camera_request *r = NULL;
        assert(!pico_camera_request_prepare(&c, &r)); fail_memory_at = fail;
        assert(pico_camera_request_submit(r) == -EIO && !order);
        assert(!pico_camera_request_complete(r)); fail_memory_at = 0;
        assert(!pico_camera_request_release(&r) && !live_buffers);
    }
    reset(); struct pico_camera_request_config c = config(); struct pico_camera_request *r = NULL;
    assert(!pico_camera_request_prepare(&c, &r)); fail_io_at = io_calls + 1;
    assert(pico_camera_request_release(&r) == -EINTR && r); unsigned count = io_calls;
    assert(pico_camera_request_release(&r) == -EIO && io_calls == count); retained = r;
    assert(frames == 300 && releases == 300 && sensor_configs == 301 && ife_configs == 300 && schedules == 302);
    printf("PICO_CAMERA_REQUEST_PASS frames=%u faults=20 uncertain_fence_retained=1\n", frames);
    return retained ? 0 : 1;
}
