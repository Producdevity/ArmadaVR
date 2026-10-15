#include "quest-camera-control.h"
#include <errno.h>
#include <sys/ioctl.h>

static int submit(int fd, uint32_t opcode, void *data, uint32_t size)
{
    if (fd < 0) return -EINVAL;
    struct { uint32_t opcode, size, type, reserved; uint64_t pointer; } command = {
        opcode, size, 1, 0, (uintptr_t)data};
    _Static_assert(sizeof(command) == 24 && sizeof(void *) == 8, "Quest camera control ABI");
    int result = ioctl(fd, 0xc01856c0UL, &command);
    return result < 0 ? -errno : result ? -EPROTO : 0;
}

int quest_camera_sensor_query(int fd, struct quest_camera_sensor_capabilities *caps)
{
    if (!caps) return -EINVAL;
    struct quest_camera_sensor_capabilities data = {0};
    _Static_assert(sizeof(data) == 40, "Quest sensor query ABI");
    int result = submit(fd, 0x101, &data, sizeof(data));
    if (!result) *caps = data;
    return result;
}

int quest_camera_phy_query(int fd, struct quest_camera_phy_capabilities *caps)
{
    if (!caps) return -EINVAL;
    uint32_t data[4] = {0};
    int result = submit(fd, 0x101, data, sizeof(data));
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
    int result = submit(fd, 0x101, &envelope, sizeof(envelope));
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

int quest_camera_session_create(int fd, int32_t *session)
{
    if (!session || *session) return -EINVAL;
    int32_t data[2] = {0};
    int result = submit(fd, 0x10f, data, sizeof(data));
    if (data[0] > 0) *session = data[0];
    return result ? result : data[0] > 0 ? 0 : -EPROTO;
}

int quest_camera_session_destroy(int fd, int32_t *session)
{
    if (!session || *session <= 0) return -EINVAL;
    int32_t data[2] = {*session, 0};
    int result = submit(fd, 0x110, data, sizeof(data));
    if (!result) *session = 0;
    return result;
}

int quest_camera_link_create(int fd, int32_t session, const int32_t *devices, size_t count, int32_t *link)
{
    if (session <= 0 || !devices || count < 2 || count > 8 || !link || *link) return -EINVAL;
    struct { int32_t session; uint32_t count; int32_t devices[64], link; } data = {0};
    _Static_assert(sizeof(data) == 268, "Quest CRM v1 link ABI");
    for (size_t i = 0; i < count; ++i) {
        if (devices[i] <= 0) return -EINVAL;
        for (size_t j = 0; j < i; ++j) if (devices[i] == devices[j]) return -EINVAL;
        data.devices[i] = devices[i];
    }
    data.session = session; data.count = (uint32_t)count;
    int result = submit(fd, 0x111, &data, sizeof(data));
    if (data.link > 0) *link = data.link;
    return result ? result : data.link > 0 ? 0 : -EPROTO;
}

int quest_camera_link_destroy(int fd, int32_t session, int32_t *link)
{
    if (session <= 0 || !link || *link <= 0) return -EINVAL;
    int32_t data[2] = {session, *link};
    int result = submit(fd, 0x112, data, sizeof(data));
    if (!result) *link = 0;
    return result;
}

int quest_camera_link_activate(int fd, int32_t session, int32_t link, bool active)
{
    if (session <= 0 || link <= 0) return -EINVAL;
    int32_t data[8] = {active ? 0 : 1, session, 1, 0, 0, 0, link, 0};
    return submit(fd, 0x11a, data, sizeof(data));
}

int quest_camera_links_synchronize(int fd, int32_t session, const int32_t links[2], int32_t master)
{
    if (session <= 0 || !links || links[0] <= 0 || links[1] <= 0 || links[0] == links[1] ||
        (master != links[0] && master != links[1])) return -EINVAL;
    int32_t data[7] = {session, 1, 2, links[0], links[1], master, 0};
    return submit(fd, 0x115, data, sizeof(data));
}

int quest_camera_request_schedule(int fd, int32_t session, int32_t link, uint64_t request,
                                  bool synchronized, bool bubble_recovery)
{
    if (session <= 0 || link <= 0 || !request || request > INT64_MAX) return -EINVAL;
    struct { int32_t session, link, bubble, sync, timeout, reserved; int64_t request; } data = {
        session, link, bubble_recovery, synchronized, 0, 0, (int64_t)request};
    _Static_assert(sizeof(data) == 32, "Quest CRM schedule ABI");
    return submit(fd, 0x113, &data, sizeof(data));
}

int quest_camera_requests_flush(int fd, int32_t session, int32_t link)
{
    if (session <= 0 || link <= 0) return -EINVAL;
    struct { int32_t session, link; uint32_t type, reserved; int64_t request; } data = {session, link, 0, 0, 0};
    _Static_assert(sizeof(data) == 24, "Quest CRM flush ABI");
    return submit(fd, 0x114, &data, sizeof(data));
}
