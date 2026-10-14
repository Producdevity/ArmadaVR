#include "pico-camera-control.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

static unsigned calls, mode, failure, malformed;
static uint32_t opcode;
static uint64_t get(const void *data, size_t offset, size_t size)
{
    const uint8_t *p = (const uint8_t *)data + offset;
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static void set32(void *data, size_t offset, uint32_t value)
{
    uint8_t *p = (uint8_t *)data + offset;
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
int ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request); void *header = va_arg(args, void *); va_end(args);
    assert(fd == 21 && request == 0xc01856c0UL && get(header, 0, 4) == opcode);
    assert(!get(header, 12, 4)); ++calls;
    if (opcode == 0x10a) {
        assert(get(header, 4, 4) == 8 && get(header, 8, 4) == 2 && get(header, 16, 8) == 404);
    } else {
        assert(get(header, 8, 4) == 1);
        void *data = (void *)(uintptr_t)get(header, 16, 8);
        if (opcode == 0x101) {
            assert(get(header, 4, 4) == (mode == 0 ? 40 : 16));
            if (mode == 0) {
                for (unsigned i = 0; i < 10; ++i) set32(data, i * 4, i);
            } else if (mode == 1) {
                set32(data, 0, 5); set32(data, 4, 0x1234); set32(data, 8, 3);
            } else {
                assert(get(data, 0, 4) == 144 && get(data, 4, 4) == 1);
                data = (void *)(uintptr_t)get(data, 8, 8);
                set32(data, 0, 301); set32(data, 8, 302); set32(data, 16, malformed ? 6 : 1);
                set32(data, 24, 7); set32(data, 32, 1); set32(data, 36, 2); set32(data, 40, 3);
            }
        } else if (opcode == 0x102) {
            assert(get(header, 4, 4) == 24 && get(data, 0, 4) == 101 && !get(data, 4, 4));
            assert(get(data, 8, 4) == 1 && get(data, 12, 4) == (mode == 2 ? 0xfefefefe : 0));
            if (mode == 1) {
                const void *info = (const void *)(uintptr_t)get(data, 16, 8);
                assert(get(info, 0, 4) == 1 && !get(info, 4, 4));
            } else assert(!get(data, 16, 8));
            set32(data, 4, malformed ? 0xffffffff : 201);
        } else if (opcode == 0x151) {
            assert(get(header, 4, 4) == 120 && get(data, 0, 4) == 2 && !get(data, 4, 4));
            assert(get(data, 8, 4) == 101 && get(data, 12, 4) == 201);
            assert(get(data, 16, 4) == 1 && get(data, 20, 4) == 264);
            const uint8_t *resource = (const uint8_t *)(uintptr_t)get(data, 24, 8);
            for (unsigned i = 0; i < 264; ++i) assert(resource[i] == 0xa5);
            for (unsigned i = 32; i < 120; ++i) assert(!((uint8_t *)data)[i]);
            set32(data, 32, 2); set32(data, 52, 4); set32(data, 112, 1);
        } else {
            assert(opcode == 0x152 && get(header, 4, 4) == 16);
            assert(get(data, 0, 4) == 1 && !get(data, 4, 4));
            assert(get(data, 8, 4) == 101 && get(data, 12, 4) == 201);
        }
    }
    if (failure) { errno = EIO; return -1; }
    return 0;
}
int main(void)
{
    opcode = 0x101;
    struct pico_camera_sensor_capabilities sensor = {0};
    assert(!pico_camera_sensor_query(21, &sensor) && sensor.slot == 0 && sensor.phy_slot == 9 && sensor.yaw == 4);
    mode = 1; struct pico_camera_phy_capabilities phy = {0};
    assert(!pico_camera_phy_query(21, &phy) && phy.slot == 5 && phy.version == 0x1234 && phy.clock_lane == 3);
    mode = 2; struct pico_camera_ife_capabilities ife = {0};
    assert(!pico_camera_ife_query(21, &ife) && ife.device_mmu == 301 && ife.cdm_mmu == 302 && ife.device_count == 1);
    assert(ife.devices[0].type == 7 && ife.devices[0].major == 1 && ife.devices[0].minor == 2 && ife.devices[0].increment == 3);
    struct pico_camera_ife_capabilities saved = ife;
    malformed = 1; assert(pico_camera_ife_query(21, &ife) == -EPROTO && !memcmp(&ife, &saved, sizeof(ife)));
    malformed = 0; failure = 1;
    assert(pico_camera_ife_query(21, &ife) == -EIO && !memcmp(&ife, &saved, sizeof(ife)));
    sensor.slot = 99; mode = 0;
    assert(pico_camera_sensor_query(21, &sensor) == -EIO && sensor.slot == 99);
    phy.slot = 99; mode = 1;
    assert(pico_camera_phy_query(21, &phy) == -EIO && phy.slot == 99);
    failure = 0; opcode = 0x10a; assert(!pico_camera_sensor_probe_submit(21, 404));
    opcode = 0x102;
    for (mode = 0; mode < 3; ++mode) for (failure = 0; failure < 2; ++failure) {
        int32_t device = 0;
        int result = mode == 0 ? pico_camera_sensor_acquire(21, 101, &device) :
                     mode == 1 ? pico_camera_phy_acquire(21, 101, true, &device) :
                                 pico_camera_ife_device_acquire(21, 101, &device);
        assert(result == (failure ? -EIO : 0) && device == 201);
    }
    failure = 0; malformed = 1; mode = 0; int32_t device = 0;
    assert(pico_camera_sensor_acquire(21, 101, &device) == -EPROTO && !device);
    malformed = 0;
    uint8_t resource[264]; memset(resource, 0xa5, sizeof(resource));
    struct pico_camera_ife_hardware hardware = {0}; opcode = 0x151;
    assert(!pico_camera_ife_hardware_acquire(21, 101, 201, resource, sizeof(resource), &hardware));
    assert(hardware.id[0] == 2 && hardware.paths[0][0] == 4 && hardware.valid == 1);
    hardware.id[0] = 99; failure = 1;
    assert(pico_camera_ife_hardware_acquire(21, 101, 201, resource, sizeof(resource), &hardware) == -EIO && hardware.id[0] == 99);
    failure = 0; opcode = 0x152; assert(!pico_camera_ife_hardware_release(21, 101, 201));
    unsigned before = calls;
    assert(pico_camera_sensor_probe_submit(-1, 404) == -EINVAL);
    assert(pico_camera_sensor_probe_submit(21, 0) == -EINVAL);
    assert(pico_camera_sensor_acquire(21, 0, &device) == -EINVAL);
    device = 201; assert(pico_camera_sensor_acquire(21, 101, &device) == -EINVAL && device == 201);
    assert(pico_camera_phy_acquire(21, 101, false, NULL) == -EINVAL);
    assert(pico_camera_ife_hardware_acquire(21, 101, 201, resource, 263, &hardware) == -EINVAL);
    assert(pico_camera_ife_hardware_acquire(21, 101, 201, NULL, 264, &hardware) == -EINVAL);
    assert(pico_camera_ife_query(21, NULL) == -EINVAL);
    assert(calls == before);
    printf("PICO_CAMERA_ACQUIRE_PASS calls=%u hardware=0\n", calls);
    return 0;
}
