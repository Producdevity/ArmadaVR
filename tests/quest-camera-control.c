#include "quest-camera-control.h"
#include <sys/types.h>
#include <media/cam_defs.h>
#include <media/cam_sensor.h>
#include <media/cam_isp.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static unsigned calls;
static int injected_error, device_count = 8;
static void zero(const void *p, size_t size)
{
    const unsigned char *bytes = p;
    for (size_t i = 0; i < size; ++i) assert(!bytes[i]);
}
int ioctl(int fd, unsigned long request, ...)
{
    assert(request == VIDIOC_CAM_CONTROL && fd >= 11 && fd <= 13);
    va_list ap; va_start(ap, request); struct cam_control *c = va_arg(ap, void *); va_end(ap);
    assert(c->op_code == CAM_QUERY_CAP && c->handle_type == CAM_HANDLE_USER_POINTER && !c->reserved);
    assert(c->handle); ++calls;
    void *p = (void *)(uintptr_t)c->handle;
    if (fd == 11) {
        assert(c->size == sizeof(struct cam_sensor_query_cap)); zero(p, c->size);
        struct cam_sensor_query_cap data = {4, 0, 90, 180, 270, 9, 8, 7, 6, 2};
        memcpy(p, &data, sizeof(data));
    } else if (fd == 12) {
        assert(c->size == sizeof(struct cam_csiphy_query_cap)); zero(p, c->size);
        struct cam_csiphy_query_cap data = {2, 0x103, 4, 0}; memcpy(p, &data, sizeof(data));
    } else {
        assert(c->size == sizeof(struct cam_query_cap_cmd));
        struct cam_query_cap_cmd *q = p;
        assert(q->size == sizeof(struct cam_isp_query_cap_cmd) && q->handle_type == CAM_HANDLE_USER_POINTER && q->caps_handle);
        struct cam_isp_query_cap_cmd *caps = (void *)(uintptr_t)q->caps_handle;
        assert(caps->device_iommu.non_secure == -1 && caps->device_iommu.secure == -1);
        assert(caps->cdm_iommu.non_secure == -1 && caps->cdm_iommu.secure == -1);
        assert(!caps->num_dev && !caps->reserved); zero(caps->dev_caps, sizeof(caps->dev_caps));
        caps->device_iommu.non_secure = 99; caps->cdm_iommu.non_secure = 98;
        caps->num_dev = device_count;
        for (unsigned i = 0; i < CAM_ISP_HW_MAX; ++i) {
            caps->dev_caps[i].hw_type = i; caps->dev_caps[i].num_hw = i + 1;
            caps->dev_caps[i].hw_version = (struct cam_hw_version){i + 2, i + 3, i + 4, 0};
        }
    }
    if (injected_error > 0) { errno = injected_error; return -1; }
    return injected_error ? 1 : 0;
}
int main(void)
{
    _Static_assert(CAM_ISP_HW_MAX == 8, "Quest has eight ISP hardware classes");
    _Static_assert(sizeof(struct cam_isp_query_cap_cmd) == 216, "Quest ISP payload");
    _Static_assert(sizeof(struct cam_sensor_query_cap) == sizeof(struct quest_camera_sensor_capabilities), "sensor payload");
    _Static_assert(offsetof(struct cam_sensor_query_cap, csiphy_slot_id) == offsetof(struct quest_camera_sensor_capabilities, phy_slot), "sensor PHY slot");
    struct quest_camera_sensor_capabilities sensor, old_sensor;
    struct quest_camera_phy_capabilities phy, old_phy;
    struct quest_camera_isp_capabilities isp, old_isp;
    assert(!quest_camera_sensor_query(11, &sensor));
    assert(sensor.slot == 4 && !sensor.secure && sensor.pitch == 90 && sensor.roll == 180 && sensor.yaw == 270);
    assert(sensor.actuator_slot == 9 && sensor.eeprom_slot == 8 && sensor.ois_slot == 7 && sensor.flash_slot == 6 && sensor.phy_slot == 2);
    assert(!quest_camera_phy_query(12, &phy) && phy.slot == 2 && phy.version == 0x103 && phy.clock_lane == 4);
    for (device_count = 0; device_count <= 8; ++device_count) {
        assert(!quest_camera_isp_query(13, &isp));
        assert(isp.device_count == (unsigned)device_count && isp.device_mmu == 99 && isp.cdm_mmu == 98);
        for (unsigned i = 0; i < isp.device_count; ++i) {
            assert(isp.devices[i].type == i && isp.devices[i].count == i + 1);
            assert(isp.devices[i].major == i + 2 && isp.devices[i].minor == i + 3 && isp.devices[i].increment == i + 4);
        }
        for (unsigned i = isp.device_count; i < 8; ++i) zero(&isp.devices[i], sizeof(isp.devices[i]));
    }
    old_sensor = sensor; old_phy = phy; old_isp = isp;
    int errors[] = {EIO, EFAULT, EINTR, ENODEV, -1};
    device_count = 8;
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        injected_error = errors[i]; int expected = injected_error > 0 ? -injected_error : -EPROTO;
        unsigned before = calls;
        assert(quest_camera_sensor_query(11, &sensor) == expected && !memcmp(&sensor, &old_sensor, sizeof(sensor)));
        assert(quest_camera_phy_query(12, &phy) == expected && !memcmp(&phy, &old_phy, sizeof(phy)));
        assert(quest_camera_isp_query(13, &isp) == expected && !memcmp(&isp, &old_isp, sizeof(isp)));
        assert(calls == before + 3);
    }
    injected_error = 0;
    int counts[] = {-1, 9, INT32_MAX, INT32_MIN};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        device_count = counts[i];
        assert(quest_camera_isp_query(13, &isp) == -EPROTO && !memcmp(&isp, &old_isp, sizeof(isp)));
    }
    unsigned before = calls;
    assert(quest_camera_sensor_query(-1, &sensor) == -EINVAL && quest_camera_sensor_query(11, NULL) == -EINVAL);
    assert(quest_camera_phy_query(-1, &phy) == -EINVAL && quest_camera_phy_query(12, NULL) == -EINVAL);
    assert(quest_camera_isp_query(-1, &isp) == -EINVAL && quest_camera_isp_query(13, NULL) == -EINVAL);
    assert(calls == before);
    printf("QUEST_CAMERA_QUERY_PASS ioctls=%u hardware=0 isp_entries=8\n", calls);
}
