#include "quest-camera-pipeline.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

struct action {unsigned kind, camera; bool start;};
static struct action actions[32];
static unsigned calls, fail_at;
static uint32_t get(const void *p, unsigned offset)
{
    const uint8_t *v = (const uint8_t *)p + offset;
    return v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
}
int ioctl(int fd, unsigned long op, ...)
{
    assert(op == 0xc01856c0UL && calls < 32);
    va_list args; va_start(args, op); const void *header = va_arg(args, const void *); va_end(args);
    uintptr_t pointer; memcpy(&pointer, (const uint8_t *)header + 16, 8);
    const void *data = (const void *)pointer;
    assert(get(header, 8) == 1 && !get(header, 12));
    struct action a;
    if (fd == 17) {
        assert(get(header, 0) == 0x11a && get(header, 4) == 32);
        assert(get(data, 4) == 101 && get(data, 8) == 1 && get(data, 0) <= 1);
        a = (struct action){3, get(data, 24) - 1000, get(data, 0) == 0};
    } else {
        assert(fd >= 20 && fd < 32 && get(header, 4) == 8);
        assert(get(header, 0) == 0x103 || get(header, 0) == 0x104);
        assert(get(data, 0) == 101 && get(data, 4) == (unsigned)fd + 80);
        a = (struct action){(unsigned)(fd - 20) % 3, (unsigned)(fd - 20) / 3, get(header, 0) == 0x103};
    }
    assert(a.camera < 4); actions[calls++] = a;
    if (calls == fail_at) {errno = EINTR; return -1;}
    return 0;
}
static struct quest_camera_pipeline_config config(unsigned count)
{
    struct quest_camera_pipeline_config c = {.crm_fd = 17, .session = 101, .count = count};
    for (unsigned i = 0; i < 4; ++i) {
        c.cameras[i].link = 1000 + (int)i;
        c.cameras[i].ife = (struct quest_camera_pipeline_device){20 + (int)i * 3, 100 + (int)i * 3};
        c.cameras[i].phy = (struct quest_camera_pipeline_device){21 + (int)i * 3, 101 + (int)i * 3};
        c.cameras[i].sensor = (struct quest_camera_pipeline_device){22 + (int)i * 3, 102 + (int)i * 3};
    }
    return c;
}
static void verify(unsigned count, unsigned start_count)
{
    const unsigned start_order[] = {3, 0, 1, 2}, stop_order[] = {0, 2, 1, 3};
    bool attempted[4][4] = {{false}};
    for (unsigned i = 0; i < start_count; ++i) {
        struct action a = actions[i];
        assert(a.start && a.kind == start_order[i / count] && a.camera == i % count);
        attempted[a.kind][a.camera] = true;
    }
    unsigned next = start_count;
    for (unsigned rank = 0; rank < 4; ++rank) for (unsigned camera = 0; camera < count; ++camera) {
        unsigned kind = stop_order[rank]; if (!attempted[kind][camera]) continue;
        assert(next < calls); struct action a = actions[next++];
        assert(!a.start && a.kind == kind && a.camera == camera);
    }
    assert(next == calls && calls == start_count * 2);
}
int main(void)
{
    struct quest_camera_pipeline *p = NULL; struct quest_camera_pipeline_config c = config(4);
    assert(quest_camera_pipeline_create(NULL, &p) == -EINVAL);
    c.cameras[3].sensor.handle = c.cameras[0].ife.handle;
    assert(quest_camera_pipeline_create(&c, &p) == -EINVAL);
    c = config(4); c.cameras[2].link = c.cameras[1].link;
    assert(quest_camera_pipeline_create(&c, &p) == -EINVAL);
    c = config(4); c.cameras[0].sensor.fd = c.crm_fd;
    assert(quest_camera_pipeline_create(&c, &p) == -EINVAL);
    c = config(5); assert(quest_camera_pipeline_create(&c, &p) == -EINVAL && !calls);
    c = config(1); assert(!quest_camera_pipeline_create(&c, &p));
    assert(!quest_camera_pipeline_stop(p) && quest_camera_pipeline_start(p) == -EINVAL);
    assert(!quest_camera_pipeline_destroy(&p) && !calls);
    for (unsigned cycle = 0; cycle < 150; ++cycle) for (unsigned count = 1; count <= 4; ++count) {
        calls = fail_at = 0; c = config(count);
        assert(!quest_camera_pipeline_create(&c, &p));
        assert(!quest_camera_pipeline_start(p));
        assert(quest_camera_pipeline_destroy(&p) == -EBUSY);
        assert(quest_camera_pipeline_start(p) == -EINVAL);
        assert(!quest_camera_pipeline_stop(p)); verify(count, count * 4);
        assert(!quest_camera_pipeline_stop(p)); verify(count, count * 4);
        assert(quest_camera_pipeline_destroy(&p) == -EBUSY);
        assert(!quest_camera_pipeline_quiesced(p));
        assert(!quest_camera_pipeline_destroy(&p) && !p);
    }
    unsigned failures = 0;
    for (unsigned count = 1; count <= 4; ++count) for (unsigned fault = 1; fault <= count * 4; ++fault) {
        calls = 0; fail_at = fault; c = config(count);
        assert(!quest_camera_pipeline_create(&c, &p));
        assert(quest_camera_pipeline_start(p) == -EINTR && calls == fault);
        assert(quest_camera_pipeline_start(p) == -EINVAL);
        fail_at = 0; assert(!quest_camera_pipeline_stop(p)); verify(count, fault);
        assert(quest_camera_pipeline_destroy(&p) == -EBUSY);
        assert(!quest_camera_pipeline_quiesced(p));
        assert(!quest_camera_pipeline_destroy(&p) && !p); ++failures;
    }
    unsigned retained_count = 0;
    for (unsigned count = 1; count <= 4; ++count) for (unsigned fault = 1; fault <= count * 4; ++fault) {
        calls = fail_at = 0; c = config(count);
        assert(!quest_camera_pipeline_create(&c, &p) && !quest_camera_pipeline_start(p));
        fail_at = calls + fault; assert(quest_camera_pipeline_stop(p) == -EINTR);
        verify(count, count * 4);
        assert(quest_camera_pipeline_stop(p) == -EINTR && quest_camera_pipeline_destroy(&p) == -EBUSY);
        verify(count, count * 4);
        assert(!quest_camera_pipeline_quiesced(p));
        assert(quest_camera_pipeline_destroy(&p) == -EBUSY);
        quest_camera_pipeline_closed(&p);assert(!p);++retained_count;++failures;
    }
    assert(failures == 80 && retained_count == 40);
    puts("QUEST_CAMERA_PIPELINE_PASS cycles=600 faults=80 uncertain_owners=40");
    return 0;
}
