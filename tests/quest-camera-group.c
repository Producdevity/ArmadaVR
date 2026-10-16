#include "quest-camera-group.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#ifdef QUEST_CAMERA_VENDOR_TEST
#include <media/cam_defs.h>
#include <media/cam_req_mgr.h>
#include <media/cam_isp.h>
_Static_assert(CAM_REQ_MGR_CREATE_SESSION == 0x10f && CAM_REQ_MGR_DESTROY_SESSION == 0x110, "session opcodes");
_Static_assert(CAM_REQ_MGR_LINK_V3 == 0x11d && CAM_REQ_MGR_SYNC_MODE_V2 == 0x11f, "link opcodes");
_Static_assert(CAM_ISP_IFE0_HW == 1 && CAM_ISP_IFE2_HW == 0x100 && CAM_ISP_IFE9_LITE_HW == 0x80000, "IFE mask");
_Static_assert(CAM_ISP_RDI0_PATH == 8 && CAM_ISP_RDI3_PATH == 64, "RDI paths");
#endif

static unsigned calls, setup_calls, cleanup_calls, fail_setup, fail_cleanup, malformed, cycles, faults;
static bool cleaning, copy_handle, crm_live, device_live[4][3], hardware_live[4], link_live[4], lost;
static unsigned hardware_count, camera_count, master, variant;
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
    for (unsigned i = 0; i < 4; ++i)
        if (link_live[i] || hardware_live[i] || device_live[i][0] || device_live[i][1] || device_live[i][2]) return true;
    return false;
}
static unsigned hw_id(unsigned i)
{
    const unsigned ids[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 0x10000, 0x20000, 0x40000, 0x80000};
    return ids[(variant + (variant & 1 ? 0 : i)) % 13];
}
static unsigned rdi(unsigned i) { return variant & 1 ? i : variant % 4; }
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
        if (fd >= 20 && fd < 28 && !(fd % 2)) {
            unsigned i = (fd - 20) / 2;
            assert(get(header, 4, 4) == 40);
            put(data, 0, i); put(data, 36, i);
            if (malformed == 1 && !i) put(data, 0, 5);
            if (malformed == 2 && !i) put(data, 36, 5);
            if (malformed == 3 && !i) put(data, 4, 1);
        } else if (fd >= 21 && fd < 28) {
            unsigned i = (fd - 21) / 2;
            assert(get(header, 4, 4) == 16);
            put(data, 0, i); put(data, 4, malformed == 4 ? 0 : 0x201);
        } else {
            assert(fd == 11 && get(header, 4, 4) == 16 && get(data, 0, 4) == 216);
            void *caps = (void *)(uintptr_t)get(data, 8, 8);
            put(caps, 0, malformed == 5 ? UINT32_MAX : 30); put(caps, 8, 31);
            put(caps, 16, 2); put(caps, 24, 1); put(caps, 28, 580);
            put(caps, 48, 1); put(caps, 52, 580);
        }
    } else if (op == 0x10f) {
        assert(!cleaning && fd == 10 && !crm_live); crm_live = true;
        if (malformed == 13) lost = true;
        else if (!failed || copy_handle) put(data, 0, 100); else lost = true;
    } else if (op == 0x102) {
        assert(!cleaning && crm_live && get(data, 0, 4) == 100 && !get(data, 4, 4));
        unsigned i, kind;
        if (fd == 11) { i = hardware_count; kind = 2; assert(get(data, 12, 4) == 0xfefefefe); }
        else { assert(fd >= 20 && fd < 28); i = (fd - 20) / 2; kind = (fd - 20) % 2; assert(!get(data, 12, 4)); }
        assert(i < camera_count && !device_live[i][kind]);
        if (kind == 1) {
            const void *info = (void *)(uintptr_t)get(data, 16, 8);
            assert(!get(info, 0, 6) && get(info, 6, 1) == variant % 2 && !get(info, 7, 1));
        } else assert(!get(data, 16, 8));
        device_live[i][kind] = true;
        if (malformed == 9 && i == 1 && !kind) { put(data, 4, 200); lost = true; }
        else if (malformed == 12 && !i && !kind) lost = true;
        else if (!failed || copy_handle) put(data, 4, 200 + i * 10 + kind); else lost = true;
    } else if (op == 0x151) {
        assert(!cleaning && fd == 11 && crm_live && get(data, 8, 4) == 100 && get(data, 20, 4) == 216);
        unsigned i = hardware_count++; assert(i < camera_count && device_live[i][2]);
        assert(get(data, 12, 4) == 202 + i * 10);
        const void *resource = (void *)(uintptr_t)get(data, 24, 8);
        assert(get(resource, 24, 4) == 0x4001 + i && get(resource, 56, 4) == 0x2a);
        assert(get(resource, 176, 4) == 0x3006 + rdi(i));
        hardware_live[i] = true;
        put(data, 32, malformed == 8 ? 0x1000 : malformed == 7 ? hw_id(0) : hw_id(i));
        put(data, 52, malformed == 6 ? 4 : 8u << rdi(i)); put(data, 112, 1);
        if (malformed == 11) put(data, 36, 2);
    } else if (op == 0x11d) {
        assert(!cleaning && fd == 10 && crm_live && get(data, 0, 4) == 100 && get(data, 4, 4) == 2);
        assert(get(header, 4, 4) == 1040 && get(data, 1036, 4) == (1 + variant % 2 + (variant & 2 ? 4 : 0)));
        unsigned i = (get(data, 8, 4) - 200) / 10;
        assert(i < camera_count && device_live[i][0] && device_live[i][2] && hardware_live[i] && !link_live[i]);
        assert(get(data, 12, 4) == 202 + i * 10);
        link_live[i] = true;
        if (malformed == 10 && i == 1) { put(data, 1032, 300); lost = true; }
        else if (malformed == 14) lost = true;
        else if (!failed || copy_handle) put(data, 1032, 300 + i); else lost = true;
    } else if (op == 0x11f) {
        assert(!cleaning && fd == 10 && camera_count > 1);
        assert(get(data, 0, 4) == 2 && get(data, 4, 4) == camera_count && get(data, 8, 4) == 100);
        assert(get(data, 12, 4) == 1 && get(data, 16, 4) == 300 + master && !get(data, 20, 4));
        for (unsigned i = 0; i < camera_count; ++i) {
            assert(link_live[i] && get(data, 24 + i * 16, 4) == 300 + i);
            assert(get(data, 28 + i * 16, 4) == i && !get(data, 32 + i * 16, 8));
        }
    } else if (op == 0x112) {
        assert(cleaning && fd == 10 && crm_live && get(data, 0, 4) == 100);
        unsigned i = get(data, 4, 4) - 300; assert(i < camera_count && link_live[i]); link_live[i] = false;
    } else if (op == 0x152) {
        assert(cleaning && fd == 11 && crm_live && get(data, 8, 4) == 100);
        unsigned i = (get(data, 12, 4) - 202) / 10;
        assert(i < camera_count && !link_live[i] && hardware_live[i]); hardware_live[i] = false;
    } else if (op == 0x106) {
        assert(cleaning && crm_live && get(data, 0, 4) == 100);
        unsigned handle = get(data, 4, 4) - 200, i = handle / 10, kind = handle % 10;
        assert(i < camera_count && kind < 3 && device_live[i][kind] && !link_live[i]);
        assert(fd == (kind == 2 ? 11 : 20 + (int)i * 2 + (int)kind));
        if (kind == 2) assert(!hardware_live[i]);
        device_live[i][kind] = false;
    } else if (op == 0x110) {
        assert(cleaning && fd == 10 && get(data, 0, 4) == 100 && crm_live && !devices_alive()); crm_live = false;
    } else assert(!"unexpected ioctl");
    if (failed) { errno = EINTR; return -1; }
    return 0;
}
static struct quest_camera_group_config config(void)
{
    struct quest_camera_group_config c = {.crm_fd = 10, .isp_fd = 11, .count = camera_count,
        .master = master, .synchronized = camera_count > 1 && !(variant & 4), .mode = 1 + variant % 2, .independent = (variant & 2) != 0};
    for (unsigned i = 0; i < camera_count; ++i) {
        c.cameras[i].sensor_fd = 20 + i * 2; c.cameras[i].phy_fd = 21 + i * 2;
        c.cameras[i].sensor_slot = i; c.cameras[i].phy_version = 0x201;
        c.cameras[i].input = (struct quest_camera_isp_config){.phy = i, .lanes = 2, .lane_map = 0x10,
            .cphy = variant % 2, .virtual_channel = 0, .width = 640, .height = 513, .bits = 8,
            .rdi = rdi(i), .output = QUEST_CAMERA_RAW_MIPI};
    }
    return c;
}
static void run(unsigned setup_error, bool copy, unsigned cleanup_error, unsigned bad)
{
    assert(!crm_live && !devices_alive());
    calls = setup_calls = cleanup_calls = hardware_count = 0; fail_setup = setup_error; fail_cleanup = cleanup_error;
    copy_handle = copy; malformed = bad; cleaning = lost = false; master = camera_count > 1 && !(variant & 4) ? variant % camera_count : 0;
    struct quest_camera_group_config c = config();
    struct quest_camera_group *s = NULL;
    struct quest_camera_group_view view, saved; memset(&view, 0xa5, sizeof(view)); saved = view;
    int result = quest_camera_group_acquire(&c, &s);
    if (setup_error <= 2 * camera_count + 1 && setup_error) assert(result == -EINTR && !s);
    else if (bad && bad <= 5) assert(result == -ENODEV && !s);
    else {
        assert(s);
        if (setup_error || bad) {
            assert(result == (bad ? -EPROTO : -EINTR));
            assert(!quest_camera_group_view(s, &view) && !memcmp(&view, &saved, sizeof(view)));
        } else {
            assert(!result && setup_calls == 7 * camera_count + 2 + c.synchronized && quest_camera_group_view(s, &view));
            assert(view.session == 100 && view.count == camera_count && view.capabilities.device_mmu == 30 && view.capabilities.cdm_mmu == 31);
            for (unsigned i = 0; i < camera_count; ++i) {
                assert(view.cameras[i].sensor == 200 + (int)i * 10 && view.cameras[i].phy == 201 + (int)i * 10);
                assert(view.cameras[i].isp == 202 + (int)i * 10 && view.cameras[i].link == 300 + (int)i);
            }
        }
        unsigned before = calls;
        assert(quest_camera_group_release(&s) == -EBUSY && before == calls);
        assert(quest_camera_group_acquire(&c, &s) == -EINVAL && before == calls);
        memset(&c, 0xa5, sizeof(c));
        assert(!quest_camera_group_quiesced(s));
        assert(quest_camera_group_quiesced(s) == -EINVAL);
        assert(!quest_camera_group_view(s, &view));
        cleaning = true;
        result = quest_camera_group_release(&s);
        if (cleanup_error || lost) {
            assert(result == -EIO && s);
            before = calls; assert(quest_camera_group_release(&s) == -EIO && before == calls);
            crm_live = false; memset(device_live, 0, sizeof(device_live));
            memset(hardware_live, 0, sizeof(hardware_live)); memset(link_live, 0, sizeof(link_live));
            quest_camera_group_closed(&s); assert(!s && before == calls);
        } else assert(!result && !s && !crm_live && !devices_alive());
    }
    assert(!quest_camera_group_release(&s));
    if (setup_error || cleanup_error || bad) ++faults; else ++cycles;
}
int main(void)
{
    for (camera_count = 1; camera_count <= 4; ++camera_count) {
        for (variant = 0; variant < 104; ++variant) run(0, true, 0, 0);
        variant = 0;
        for (unsigned i = 1; i <= 7 * camera_count + 2 + (camera_count > 1); ++i) { run(i, true, 0, 0); run(i, false, 0, 0); }
        for (unsigned i = 1; i <= 5 * camera_count + 1; ++i) run(0, true, i, 0);
        for (unsigned i = 1; i <= 14; ++i) if (camera_count > 1 || (i != 7 && i != 9 && i != 10)) run(0, true, 0, i);
    }
    camera_count = 4; master = variant = 0;
    struct quest_camera_group_config c = config(); struct quest_camera_group *s = NULL;
    unsigned before = calls;
#define BAD(field, value) do { struct quest_camera_group_config bad = c; bad.field = value; \
    assert(quest_camera_group_acquire(&bad, &s) == -EINVAL && !s && calls == before); } while (0)
    BAD(count, 1); BAD(count, 0); BAD(count, 5); BAD(count, SIZE_MAX); BAD(master, 4); BAD(mode, 0); BAD(mode, 3);
    BAD(crm_fd, -1); BAD(isp_fd, 10); BAD(cameras[0].sensor_fd, 10);
    BAD(cameras[0].sensor_slot, 6); BAD(cameras[0].sensor_slot, 1); BAD(cameras[0].phy_version, 0);
    BAD(cameras[0].input.phy, 1); BAD(cameras[0].input.bits, 9); BAD(cameras[0].input.combo, 1);
    assert(quest_camera_group_acquire(NULL, &s) == -EINVAL);
    assert(quest_camera_group_acquire(&c, NULL) == -EINVAL);
    assert(quest_camera_group_quiesced(NULL) == -EINVAL);
    assert(quest_camera_group_release(NULL) == -EINVAL);
    assert(!quest_camera_group_view(NULL, NULL)); quest_camera_group_closed(NULL);
    printf("QUEST_CAMERA_GROUP_PASS cycles=%u faults=%u hardware=0\n", cycles, faults);
}
