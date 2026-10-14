#include "pico-camera-control.h"
#include "pico-camera-events.h"
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

struct control_command { uint32_t opcode, size, handle_type, reserved; uint64_t pointer; };
struct sync_command { uint32_t id, size, result, reserved; uint64_t pointer; };
_Static_assert(sizeof(struct control_command) == 24, "camera control ABI");
_Static_assert(sizeof(struct sync_command) == 24, "camera sync ABI");
_Static_assert(sizeof(void *) == 8, "camera userspace requires a 64-bit ABI");

static int submit(int fd, uint32_t opcode, void *data, uint32_t size, bool sync)
{
    if (fd < 0) return -EINVAL;
    int result;
    if (sync) {
        struct sync_command command = {opcode, size, 0, 0, (uintptr_t)data};
        result = ioctl(fd, 0xc01856c0UL, &command);
    } else {
        struct control_command command = {opcode, size, 1, 0, (uintptr_t)data};
        result = ioctl(fd, 0xc01856c0UL, &command);
    }
    return result < 0 ? -errno : result ? -EPROTO : 0;
}

int pico_camera_session_create(int fd, int32_t *session)
{
    if (!session || *session) return -EINVAL;
    int32_t data[2] = {0};
    int result = submit(fd, 0x10b, data, sizeof(data), false);
    if (result) return result;
    if (data[0] <= 0) return -EPROTO;
    *session = data[0];
    return 0;
}
int pico_camera_session_destroy(int fd, int32_t *session)
{
    if (!session || *session <= 0) return -EINVAL;
    int32_t data[2] = {*session, 0};
    int result = submit(fd, 0x10c, data, sizeof(data), false);
    if (!result) *session = 0;
    return result;
}
int pico_camera_link_create(int fd, int32_t session, const int32_t *devices, size_t count, int32_t *link)
{
    if (session <= 0 || !devices || count < 2 || count > 8 || !link || *link) return -EINVAL;
    struct { int32_t session; uint32_t count; int32_t devices[64], link; } data = {0};
    _Static_assert(sizeof(data) == 268, "CRM link ABI");
    for (size_t i = 0; i < count; ++i) {
        if (devices[i] <= 0) return -EINVAL;
        for (size_t j = 0; j < i; ++j) if (devices[i] == devices[j]) return -EINVAL;
        data.devices[i] = devices[i];
    }
    data.session = session; data.count = (uint32_t)count;
    int result = submit(fd, 0x10d, &data, sizeof(data), false);
    if (result) return result;
    if (data.link <= 0) return -EPROTO;
    *link = data.link;
    return 0;
}
int pico_camera_link_destroy(int fd, int32_t session, int32_t *link)
{
    if (session <= 0 || !link || *link <= 0) return -EINVAL;
    int32_t data[2] = {session, *link};
    int result = submit(fd, 0x10e, data, sizeof(data), false);
    if (!result) *link = 0;
    return result;
}
int pico_camera_link_activate(int fd, int32_t session, int32_t link, bool active)
{
    if (session <= 0 || link <= 0) return -EINVAL;
    int32_t data[8] = {active ? 0 : 1, session, 1, 0, 0, 0, link, 0};
    return submit(fd, 0x116, data, sizeof(data), false);
}
int pico_camera_links_synchronize(int fd, int32_t session, const int32_t links[2], int32_t master)
{
    if (session <= 0 || !links || links[0] <= 0 || links[1] <= 0 || links[0] == links[1] ||
        (master != links[0] && master != links[1])) return -EINVAL;
    int32_t data[7] = {session, 1, 2, links[0], links[1], master, 0};
    return submit(fd, 0x111, data, sizeof(data), false);
}
int pico_camera_request_schedule(int fd, int32_t session, int32_t link, uint64_t request,
                                 bool synchronized, bool bubble_recovery)
{
    if (session <= 0 || link <= 0 || !request || request > INT64_MAX) return -EINVAL;
    struct { int32_t session, link, bubble, sync, timeout, reserved; int64_t request; } data = {
        session, link, bubble_recovery, synchronized, 0, 0, (int64_t)request};
    _Static_assert(sizeof(data) == 32, "CRM schedule ABI");
    return submit(fd, 0x10f, &data, sizeof(data), false);
}
int pico_camera_requests_flush(int fd, int32_t session, int32_t link)
{
    if (session <= 0 || link <= 0) return -EINVAL;
    struct { int32_t session, link; uint32_t type, reserved; int64_t request; } data = {session, link, 0, 0, 0};
    _Static_assert(sizeof(data) == 24, "CRM flush ABI");
    return submit(fd, 0x110, &data, sizeof(data), false);
}
int pico_camera_device_configure(int fd, int32_t session, int32_t device,
                                 int32_t packet, uint64_t offset, uint64_t allocation_size)
{
    if (session <= 0 || device <= 0 || packet <= 0 || offset % 8 || allocation_size < 56 ||
        offset > allocation_size - 56) return -EINVAL;
    struct { int32_t session, device; uint64_t offset, packet; } data = {session, device, offset, (uint32_t)packet};
    _Static_assert(sizeof(data) == 24, "camera configure ABI");
    return submit(fd, 0x105, &data, sizeof(data), false);
}
int pico_camera_device_stream(int fd, int32_t session, int32_t device, bool start)
{
    if (session <= 0 || device <= 0) return -EINVAL;
    int32_t data[2] = {session, device};
    return submit(fd, start ? 0x103 : 0x104, data, sizeof(data), false);
}
int pico_camera_device_release(int fd, int32_t session, int32_t *device)
{
    if (session <= 0 || !device || *device <= 0) return -EINVAL;
    int32_t data[2] = {session, *device};
    int result = submit(fd, 0x106, data, sizeof(data), false);
    if (!result) *device = 0;
    return result;
}
int pico_camera_fence_create(int fd, const char *name, int32_t *fence)
{
    if (!name || !*name || !fence || *fence) return -EINVAL;
    size_t length = 0;
    while (length < 64 && name[length]) ++length;
    if (length >= 64) return -EINVAL;
    struct { char name[64]; int32_t handle; } data = {{0}, 0};
    _Static_assert(sizeof(data) == 68, "camera fence ABI");
    memcpy(data.name, name, length);
    int result = submit(fd, 0, &data, sizeof(data), true);
    if (result) return result;
    if (data.handle <= 0) return -EPROTO;
    *fence = data.handle;
    return 0;
}
int pico_camera_fence_register(int fd, int32_t fence, uint64_t generation, uint64_t request)
{
    uint64_t data[3];
    if (!pico_camera_fence_payload(fence, generation, request, data, sizeof(data))) return -EINVAL;
    return submit(fd, 4, data, sizeof(data), true);
}
int pico_camera_fence_destroy(int fd, int32_t *fence)
{
    if (!fence || *fence <= 0) return -EINVAL;
    struct { char name[64]; int32_t handle; } data = {{0}, *fence};
    int result = submit(fd, 1, &data, sizeof(data), true);
    if (!result) *fence = 0;
    return result;
}

int pico_camera_sensor_query(int fd, struct pico_camera_sensor_capabilities *caps)
{
    if (!caps) return -EINVAL;
    struct pico_camera_sensor_capabilities data = {0};
    _Static_assert(sizeof(data) == 40, "sensor query ABI");
    int result = submit(fd, 0x101, &data, sizeof(data), false);
    if (!result) *caps = data;
    return result;
}
int pico_camera_phy_query(int fd, struct pico_camera_phy_capabilities *caps)
{
    if (!caps) return -EINVAL;
    uint32_t data[4] = {0};
    int result = submit(fd, 0x101, data, sizeof(data), false);
    if (!result) *caps = (struct pico_camera_phy_capabilities){data[0], data[1], data[2]};
    return result;
}
int pico_camera_ife_query(int fd, struct pico_camera_ife_capabilities *caps)
{
    if (!caps) return -EINVAL;
    struct {
        int32_t device_mmu[2], cdm_mmu[2], count;
        uint32_t reserved;
        struct { uint32_t type, reserved, major, minor, increment, version_reserved; } devices[5];
    } data = {{-1, -1}, {-1, -1}, 0, 0, {{0}}};
    _Static_assert(sizeof(data) == 144, "ISP query ABI");
    struct { uint32_t size, type; uint64_t pointer; } query = {sizeof(data), 1, (uintptr_t)&data};
    int result = submit(fd, 0x101, &query, sizeof(query), false);
    if (result) return result;
    if (data.count < 0 || data.count > 5) return -EPROTO;
    struct pico_camera_ife_capabilities output = {0};
    output.device_mmu = data.device_mmu[0]; output.cdm_mmu = data.cdm_mmu[0];
    output.device_count = (uint32_t)data.count;
    for (int i = 0; i < data.count; ++i) {
        output.devices[i].type = data.devices[i].type;
        output.devices[i].major = data.devices[i].major;
        output.devices[i].minor = data.devices[i].minor;
        output.devices[i].increment = data.devices[i].increment;
    }
    *caps = output;
    return 0;
}
int pico_camera_sensor_probe_submit(int fd, int32_t packet)
{
    if (fd < 0 || packet <= 0) return -EINVAL;
    struct control_command command = {0x10a, 8, 2, 0, (uint32_t)packet};
    int result = ioctl(fd, 0xc01856c0UL, &command);
    return result < 0 ? -errno : result ? -EPROTO : 0;
}
static int acquire_device(int fd, int32_t session, uint32_t count, void *info, int32_t *device)
{
    if (session <= 0 || !device || *device) return -EINVAL;
    struct { int32_t session, device; uint32_t type, count; uint64_t pointer; } data = {
        session, 0, 1, count, (uintptr_t)info};
    _Static_assert(sizeof(data) == 24, "camera acquire ABI");
    int result = submit(fd, 0x102, &data, sizeof(data), false);
    if (data.device > 0) *device = data.device;
    return result ? result : data.device > 0 ? 0 : -EPROTO;
}
int pico_camera_sensor_acquire(int fd, int32_t session, int32_t *device)
{
    return acquire_device(fd, session, 0, NULL, device);
}
int pico_camera_phy_acquire(int fd, int32_t session, bool combo, int32_t *device)
{
    uint32_t info[2] = {combo, 0};
    return acquire_device(fd, session, 0, info, device);
}
int pico_camera_ife_device_acquire(int fd, int32_t session, int32_t *device)
{
    return acquire_device(fd, session, 0xfefefefe, NULL, device);
}
int pico_camera_ife_hardware_acquire(int fd, int32_t session, int32_t device,
                                    const void *resource, size_t size,
                                    struct pico_camera_ife_hardware *hardware)
{
    if (session <= 0 || device <= 0 || !resource || size != 264 || !hardware) return -EINVAL;
    struct {
        uint32_t version, reserved;
        int32_t session, device;
        uint32_t type, size;
        uint64_t pointer;
        struct pico_camera_ife_hardware hardware;
    } data;
    _Static_assert(sizeof(data) == 120, "ISP acquire hardware v2 ABI");
    memset(&data, 0, sizeof(data));
    data.version = 2; data.session = session; data.device = device;
    data.type = 1; data.size = (uint32_t)size; data.pointer = (uintptr_t)resource;
    int result = submit(fd, 0x151, &data, sizeof(data), false);
    if (!result) *hardware = data.hardware;
    return result;
}
int pico_camera_ife_hardware_release(int fd, int32_t session, int32_t device)
{
    if (session <= 0 || device <= 0) return -EINVAL;
    int32_t data[4] = {1, 0, session, device};
    return submit(fd, 0x152, data, sizeof(data), false);
}
