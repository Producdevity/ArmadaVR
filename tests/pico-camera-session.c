#include "pico-camera-session.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

static unsigned calls, setup_calls, cleanup_calls, fail_setup, fail_cleanup, malformed, cycles, faults;
static bool cleaning, copy_handle, crm_live, device_live[2][3], hardware_live[2], link_live[2];
static unsigned hardware_count, master;
static uint64_t get(const void *data, unsigned offset, unsigned size)
{
    const uint8_t *p = (const uint8_t *)data + offset; uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static void put(void *data, unsigned offset, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) ((uint8_t *)data)[offset + i] = (uint8_t)(value >> (8 * i)); }
static bool devices_alive(void)
{
    for (unsigned i = 0; i < 2; ++i)
        if (link_live[i] || hardware_live[i] || device_live[i][0] || device_live[i][1] || device_live[i][2]) return true;
    return false;
}
int ioctl(int fd, unsigned long request, ...)
{
    assert(request == 0xc01856c0UL);
    va_list args; va_start(args, request); void *header = va_arg(args, void *); va_end(args);
    assert(get(header, 8, 4) == 1 && !get(header, 12, 4));
    void *data = (void *)(uintptr_t)get(header, 16, 8);
    uint32_t op = (uint32_t)get(header, 0, 4); ++calls;
    bool failed = cleaning ? ++cleanup_calls == fail_cleanup : ++setup_calls == fail_setup;
    if (cleaning && failed) { errno = EIO; return -1; }
    if (op == 0x101) {
        assert(!cleaning && !crm_live);
        if (fd == 20 || fd == 22) {
            unsigned i = fd == 22;
            assert(get(header, 4, 4) == 40);
            put(data, 0, i + 2); put(data, 36, i + 3);
            if (malformed == 1 && !i) put(data, 0, 5);
            if (malformed == 2 && !i) put(data, 36, 5);
            if (malformed == 3 && !i) put(data, 4, 1);
        } else if (fd == 21 || fd == 23) {
            unsigned i = fd == 23;
            assert(get(header, 4, 4) == 16);
            put(data, 0, i + 3); put(data, 4, malformed == 4 ? 0 : 0x121);
        } else {
            assert(fd == 11 && get(header, 4, 4) == 16 && get(data, 0, 4) == 144);
            void *caps = (void *)(uintptr_t)get(data, 8, 8);
            put(caps, 0, malformed == 5 ? UINT32_MAX : 30); put(caps, 8, 31);
            put(caps, 16, 2); put(caps, 24, 1); put(caps, 28, 480);
            put(caps, 48, 1); put(caps, 52, 480);
        }
    } else if (op == 0x10b) {
        assert(!cleaning && fd == 10 && !crm_live); crm_live = true;
        if (!failed || copy_handle) put(data, 0, 100);
    } else if (op == 0x102) {
        assert(!cleaning && crm_live && get(data, 0, 4) == 100 && !get(data, 4, 4));
        unsigned i, kind;
        if (fd == 11) { i = hardware_count; kind = 2; assert(get(data, 12, 4) == 0xfefefefe); }
        else { assert(fd >= 20 && fd <= 23); i = (fd - 20) / 2; kind = (fd - 20) % 2; assert(!get(data, 12, 4)); }
        assert(i < 2 && !device_live[i][kind]);
        if (kind == 1) {
            const void *info = (void *)(uintptr_t)get(data, 16, 8); assert(!get(info, 0, 8));
        } else assert(!get(data, 16, 8));
        device_live[i][kind] = true;
        if (!failed || copy_handle) put(data, 4, 200 + i * 10 + kind);
    } else if (op == 0x151) {
        assert(!cleaning && fd == 11 && crm_live && get(data, 8, 4) == 100 && get(data, 20, 4) == 264);
        unsigned i = hardware_count++; assert(i < 2 && device_live[i][2]);
        assert(get(data, 12, 4) == 202 + i * 10);
        const void *resource = (void *)(uintptr_t)get(data, 24, 8);
        assert(get(resource, 24, 4) == 0x4004 + i && get(resource, 24 + 32, 4) == 0x2a);
        hardware_live[i] = true;
        put(data, 32, malformed == 7 ? 1 : 1U << i); put(data, 52, malformed == 6 ? 4 : 8); put(data, 112, 1);
    } else if (op == 0x10d) {
        assert(!cleaning && fd == 10 && crm_live && get(data, 0, 4) == 100 && get(data, 4, 4) == 2);
        unsigned i = (get(data, 8, 4) - 200) / 10;
        assert(i < 2 && device_live[i][0] && device_live[i][2] && hardware_live[i] && !link_live[i]);
        assert(get(data, 12, 4) == 202 + i * 10);
        link_live[i] = true;
        if (!failed || copy_handle) put(data, 264, 300 + i);
    } else if (op == 0x111) {
        assert(!cleaning && fd == 10 && link_live[0] && link_live[1]);
        assert(get(data, 0, 4) == 100 && get(data, 8, 4) == 2);
        assert(get(data, 12, 4) == 300 && get(data, 16, 4) == 301 && get(data, 20, 4) == 300 + master);
    } else if (op == 0x10e) {
        assert(cleaning && fd == 10 && crm_live && get(data, 0, 4) == 100);
        unsigned i = get(data, 4, 4) - 300; assert(i < 2 && link_live[i]); link_live[i] = false;
    } else if (op == 0x152) {
        assert(cleaning && fd == 11 && crm_live && get(data, 8, 4) == 100);
        unsigned i = (get(data, 12, 4) - 202) / 10;
        assert(i < 2 && !link_live[i] && hardware_live[i]); hardware_live[i] = false;
    } else if (op == 0x106) {
        assert(cleaning && crm_live && get(data, 0, 4) == 100);
        unsigned handle = get(data, 4, 4) - 200, i = handle / 10, kind = handle % 10;
        assert(i < 2 && kind < 3 && device_live[i][kind] && !link_live[i]);
        assert(fd == (kind == 2 ? 11 : 20 + (int)i * 2 + (int)kind));
        if (kind == 2) assert(!hardware_live[i]);
        device_live[i][kind] = false;
    } else if (op == 0x10c) {
        assert(cleaning && fd == 10 && get(data, 0, 4) == 100 && crm_live && !devices_alive()); crm_live = false;
    } else assert(!"unexpected ioctl");
    if (failed) { errno = EINTR; return -1; }
    return 0;
}
static struct pico_camera_session_config config(void)
{
    struct pico_camera_session_config c = {.crm_fd = 10, .ife_fd = 11, .master = master};
    for (unsigned i = 0; i < 2; ++i) {
        c.cameras[i].sensor_fd = 20 + i * 2; c.cameras[i].phy_fd = 21 + i * 2;
        c.cameras[i].sensor_slot = 2 + i; c.cameras[i].phy_version = 0x121;
        c.cameras[i].input = (struct pico_camera_ife_config){.phy = i + 3, .lanes = 2, .lane_map = 0x3210,
            .virtual_channel = 0, .data_type = 0x2a, .width = 640, .height = 400, .bits = 8, .output = PICO_CAMERA_RAW_MIPI};
    }
    return c;
}
static void run(unsigned setup_error, bool copy, unsigned cleanup_error, unsigned bad)
{
    assert(!crm_live && !devices_alive());
    calls = setup_calls = cleanup_calls = hardware_count = 0; fail_setup = setup_error; fail_cleanup = cleanup_error;
    copy_handle = copy; malformed = bad; cleaning = false; master = cycles % 2;
    struct pico_camera_session_config c = config();
    struct pico_camera_session *s = NULL;
    struct pico_camera_session_view view, saved; memset(&view, 0xa5, sizeof(view)); saved = view;
    int result = pico_camera_session_acquire(&c, &s);
    if (setup_error <= 5 && setup_error) assert(result == -EINTR && !s);
    else if (bad && bad <= 5) assert(result == -ENODEV && !s);
    else {
        assert(s);
        if (setup_error || bad) {
            assert(result == (bad ? -EPROTO : -EINTR));
            assert(!pico_camera_session_view(s, &view) && !memcmp(&view, &saved, sizeof(view)));
        } else {
            assert(!result && setup_calls == 17 && pico_camera_session_view(s, &view));
            assert(view.session == 100 && view.capabilities.device_mmu == 30 && view.capabilities.cdm_mmu == 31);
            for (unsigned i = 0; i < 2; ++i) {
                assert(view.cameras[i].sensor == 200 + (int)i * 10 && view.cameras[i].phy == 201 + (int)i * 10);
                assert(view.cameras[i].ife == 202 + (int)i * 10 && view.cameras[i].link == 300 + (int)i);
            }
        }
        unsigned before = calls;
        assert(pico_camera_session_release(&s) == -EBUSY && before == calls);
        assert(pico_camera_session_acquire(&c, &s) == -EINVAL && before == calls);
        memset(&c, 0xa5, sizeof(c));
        assert(!pico_camera_session_quiesced(s));
        assert(pico_camera_session_quiesced(s) == -EINVAL);
        assert(!pico_camera_session_view(s, &view));
        cleaning = true;
        result = pico_camera_session_release(&s);
        bool lost = !copy && (setup_error == 6 || setup_error == 7 || setup_error == 8 || setup_error == 9 ||
                            setup_error == 11 || setup_error == 12 || setup_error == 13 || setup_error == 15 || setup_error == 16);
        if (cleanup_error || lost) {
            assert(result == -EIO && s);
            before = calls; assert(pico_camera_session_release(&s) == -EIO && before == calls);
            /* The fixture closes the entire isolated kernel session. */
            crm_live = false; memset(device_live, 0, sizeof(device_live));
            memset(hardware_live, 0, sizeof(hardware_live)); memset(link_live, 0, sizeof(link_live));
            pico_camera_session_closed(&s); assert(!s && before == calls);
        } else assert(!result && !s && !crm_live && !devices_alive());
    }
    assert(!pico_camera_session_release(&s));
    if (setup_error || cleanup_error || bad) ++faults; else ++cycles;
}
int main(void)
{
    for (unsigned i = 0; i < 300; ++i) run(0, true, 0, 0);
    for (unsigned i = 1; i <= 17; ++i) { run(i, true, 0, 0); run(i, false, 0, 0); }
    for (unsigned i = 1; i <= 11; ++i) run(0, true, i, 0);
    for (unsigned i = 1; i <= 7; ++i) run(0, true, 0, i);
    struct pico_camera_session_config c = config(); struct pico_camera_session *s = NULL;
    unsigned before = calls;
#define BAD(field, value) do { struct pico_camera_session_config bad = c; bad.field = value; \
    assert(pico_camera_session_acquire(&bad, &s) == -EINVAL && !s && calls == before); } while (0)
    BAD(master, 2); BAD(crm_fd, -1); BAD(ife_fd, 10); BAD(cameras[0].sensor_fd, 10);
    BAD(cameras[0].sensor_slot, 6); BAD(cameras[0].sensor_slot, 3); BAD(cameras[0].phy_version, 0);
    BAD(cameras[0].input.phy, 4); BAD(cameras[0].input.bits, 9);
    assert(pico_camera_session_acquire(NULL, &s) == -EINVAL);
    assert(pico_camera_session_acquire(&c, NULL) == -EINVAL);
    assert(pico_camera_session_quiesced(NULL) == -EINVAL);
    assert(pico_camera_session_release(NULL) == -EINVAL);
    assert(!pico_camera_session_view(NULL, NULL)); pico_camera_session_closed(NULL);
    printf("PICO_CAMERA_SESSION_PASS cycles=%u faults=%u hardware=0\n", cycles, faults);
}
