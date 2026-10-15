#include "quest-camera-control.h"
#include <errno.h>
#include <sys/ioctl.h>

static int query(int fd, void *data, uint32_t size)
{
    if (fd < 0) return -EINVAL;
    struct { uint32_t opcode, size, type, reserved; uint64_t pointer; } command = {
        0x101, size, 1, 0, (uintptr_t)data};
    _Static_assert(sizeof(command) == 24 && sizeof(void *) == 8, "Quest camera control ABI");
    int result = ioctl(fd, 0xc01856c0UL, &command);
    return result < 0 ? -errno : result ? -EPROTO : 0;
}

int quest_camera_sensor_query(int fd, struct quest_camera_sensor_capabilities *caps)
{
    if (!caps) return -EINVAL;
    struct quest_camera_sensor_capabilities data = {0};
    _Static_assert(sizeof(data) == 40, "Quest sensor query ABI");
    int result = query(fd, &data, sizeof(data));
    if (!result) *caps = data;
    return result;
}

int quest_camera_phy_query(int fd, struct quest_camera_phy_capabilities *caps)
{
    if (!caps) return -EINVAL;
    uint32_t data[4] = {0};
    int result = query(fd, data, sizeof(data));
    if (!result) *caps = (struct quest_camera_phy_capabilities){data[0], data[1], data[2]};
    return result;
}

int quest_camera_isp_query(int fd, struct quest_camera_isp_capabilities *caps)
{
    if (!caps) return -EINVAL;
    struct {
        int32_t device_mmu[2], cdm_mmu[2], count;
        uint32_t reserved;
        struct { uint32_t type, count, major, minor, increment, reserved; } devices[8];
    } data = {{-1, -1}, {-1, -1}, 0, 0, {{0}}};
    _Static_assert(sizeof(data) == 216, "Quest ISP query ABI");
    struct { uint32_t size, type; uint64_t pointer; } envelope = {sizeof(data), 1, (uintptr_t)&data};
    _Static_assert(sizeof(envelope) == 16, "Quest query pointer ABI");
    int result = query(fd, &envelope, sizeof(envelope));
    if (result) return result;
    if (data.count < 0 || data.count > 8) return -EPROTO;
    struct quest_camera_isp_capabilities output = {0};
    output.device_mmu = data.device_mmu[0]; output.cdm_mmu = data.cdm_mmu[0];
    output.device_count = (uint32_t)data.count;
    for (int i = 0; i < data.count; ++i) {
        output.devices[i].type = data.devices[i].type;
        output.devices[i].count = data.devices[i].count;
        output.devices[i].major = data.devices[i].major;
        output.devices[i].minor = data.devices[i].minor;
        output.devices[i].increment = data.devices[i].increment;
    }
    *caps = output;
    return 0;
}
