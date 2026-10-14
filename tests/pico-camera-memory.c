#include "pico-camera-control.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

static unsigned calls, failure, malformed, count, flags, opcode;
static uint64_t length;
static int dma_fd = 41;
static uint64_t get(const void *data, unsigned offset, unsigned size)
{
    const uint8_t *p = (const uint8_t *)data + offset; uint64_t result = 0;
    for (unsigned i = 0; i < size; ++i) result |= (uint64_t)p[i] << (i * 8);
    return result;
}
static void set(void *data, unsigned offset, uint64_t value, unsigned size)
{
    uint8_t *p = (uint8_t *)data + offset;
    for (unsigned i = 0; i < size; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
int ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request); void *command = va_arg(args, void *); va_end(args);
    assert(fd == 31 && request == 0xc01856c0UL && get(command, 0, 4) == opcode);
    assert(get(command, 8, 4) == 1 && !get(command, 12, 4)); ++calls;
    void *data = (void *)(uintptr_t)get(command, 16, 8);
    uint32_t handle = ((uint32_t)dma_fd << 16) | 7;
    if (opcode == 0x114) {
        assert(get(command, 4, 4) == 8 && get(data, 0, 4) == handle && !get(data, 4, 4));
    } else {
        unsigned base = opcode == 0x112 ? 16 : 0;
        assert(get(command, 4, 4) == (base ? 104 : 96));
        for (unsigned i = 0; i < 16; ++i) assert(get(data, base + i * 4, 4) == (i < count ? 100 + i : 0));
        assert(get(data, base + 64, 4) == count && get(data, base + 68, 4) == flags);
        if (base) {
            assert(get(data, 0, 8) == length && get(data, 8, 8) == 4096);
            assert(!get(data, 88, 4) && get(data, 92, 4) == UINT32_MAX && !get(data, 96, 8));
            set(data, 88, malformed == 1 ? 0 : handle, 4);
            set(data, 92, malformed == 2 ? UINT32_MAX : (uint32_t)dma_fd, 4);
            if (malformed == 3) set(data, 96, 1, 8);
        } else {
            assert(get(data, 72, 4) == (uint32_t)dma_fd && !get(data, 76, 4) && !get(data, 80, 8) && !get(data, 88, 8));
            set(data, 80, malformed == 1 ? 0 : handle, 4);
            if (malformed == 3) set(data, 88, 1, 8);
        }
    }
    if (failure) { errno = EINTR; return -1; }
    return 0;
}
int main(void)
{
    int32_t mmus[17]; for (unsigned i = 0; i < 17; ++i) mmus[i] = 100 + i;
    for (unsigned variant = 0; variant < 4; ++variant) for (unsigned cached = 0; cached < 2; ++cached) {
        enum pico_camera_memory_kind kind = variant ? PICO_CAMERA_MEMORY_COMMAND : PICO_CAMERA_MEMORY_PACKET;
        enum pico_camera_memory_mapping mapping = variant < 2 ? PICO_CAMERA_MEMORY_CPU :
            variant == 2 ? PICO_CAMERA_MEMORY_IO : PICO_CAMERA_MEMORY_SHARED;
        count = mapping == PICO_CAMERA_MEMORY_CPU ? 0 : 16;
        flags = 8 | 16 | 8192 | (variant ? 64 : 512) | (cached ? 1024 : 0) |
                (count ? 1 : 0) | (mapping == PICO_CAMERA_MEMORY_SHARED ? 2048 : 0);
        for (unsigned big = 0; big < 2; ++big) {
            length = big ? 0x7ffff000 : 4096; opcode = 0x112;
            struct pico_camera_memory_allocation allocation = {.fd = -1};
            assert(!pico_camera_memory_allocate(31, kind, mapping, length, count ? mmus : NULL, count, cached, &allocation));
            assert(allocation.handle == ((41 << 16) | 7) && allocation.fd == 41);
            opcode = 0x114; assert(!pico_camera_memory_release(31, &allocation.handle) && !allocation.handle && allocation.fd == 41);
        }
    }
    for (dma_fd = 0; dma_fd <= 41; dma_fd += 41) for (count = 1; count <= 16; count += 15) {
        flags = 1 | 16 | 128 | 8192; opcode = 0x113; int32_t handle = 0;
        assert(!pico_camera_pixels_import(31, dma_fd, mmus, count, false, &handle));
        assert(handle == ((dma_fd << 16) | 7)); opcode = 0x114;
        failure = 1; assert(pico_camera_memory_release(31, &handle) == -EINTR && handle);
        failure = 0; assert(!pico_camera_memory_release(31, &handle) && !handle);
    }
    dma_fd = 41; count = 0; flags = 8 | 16 | 512 | 8192; length = 4096; opcode = 0x112;
    for (malformed = 1; malformed <= 3; ++malformed) {
        struct pico_camera_memory_allocation allocation = {.fd = -1};
        assert(pico_camera_memory_allocate(31, PICO_CAMERA_MEMORY_PACKET, PICO_CAMERA_MEMORY_CPU, length, NULL, 0, false, &allocation) == -EPROTO);
        assert(allocation.fd == (malformed == 2 ? -1 : 41));
        assert(allocation.handle == (malformed == 1 ? 0 : ((41 << 16) | 7)));
    }
    malformed = 0; failure = 1; struct pico_camera_memory_allocation allocation = {.fd = -1};
    unsigned before = calls;
    assert(pico_camera_memory_allocate(31, PICO_CAMERA_MEMORY_PACKET, PICO_CAMERA_MEMORY_CPU, length, NULL, 0, false, &allocation) == -EINTR);
    assert(calls == before + 1 && allocation.fd == 41 && allocation.handle == ((41 << 16) | 7));
    count = 1; flags = 1 | 16 | 128 | 8192 | 1024; opcode = 0x113; int32_t handle = 0;
    assert(pico_camera_pixels_import(31, 41, mmus, count, true, &handle) == -EINTR && handle == ((41 << 16) | 7));
    failure = 0; malformed = 3; handle = 0;
    assert(pico_camera_pixels_import(31, 41, mmus, count, true, &handle) == -EPROTO && handle == ((41 << 16) | 7));
    malformed = 0; before = calls; allocation = (struct pico_camera_memory_allocation){.fd = -1}; handle = 0;
    assert(pico_camera_memory_allocate(31, PICO_CAMERA_MEMORY_PACKET, PICO_CAMERA_MEMORY_CPU, 0, NULL, 0, false, &allocation) == -EINVAL);
    assert(pico_camera_memory_allocate(31, PICO_CAMERA_MEMORY_PACKET, PICO_CAMERA_MEMORY_CPU, 4095, NULL, 0, false, &allocation) == -EINVAL);
    assert(pico_camera_memory_allocate(31, PICO_CAMERA_MEMORY_PACKET, PICO_CAMERA_MEMORY_CPU, 0x80000000, NULL, 0, false, &allocation) == -EINVAL);
    assert(pico_camera_memory_allocate(31, PICO_CAMERA_MEMORY_PACKET, PICO_CAMERA_MEMORY_IO, 4096, mmus, 1, false, &allocation) == -EINVAL);
    assert(pico_camera_memory_allocate(31, PICO_CAMERA_MEMORY_COMMAND, PICO_CAMERA_MEMORY_IO, 4096, NULL, 0, false, &allocation) == -EINVAL);
    assert(pico_camera_pixels_import(31, 41, mmus, 17, false, &handle) == -EINVAL);
    assert(pico_camera_pixels_import(31, 32768, mmus, 1, false, &handle) == -EINVAL);
    assert(pico_camera_pixels_import(31, 41, mmus, 0, false, &handle) == -EINVAL);
    mmus[1] = mmus[0]; assert(pico_camera_pixels_import(31, 41, mmus, 2, false, &handle) == -EINVAL);
    mmus[0] = -1; assert(pico_camera_pixels_import(31, 41, mmus, 1, false, &handle) == -EINVAL);
    assert(calls == before);
    printf("PICO_CAMERA_MEMORY_PASS calls=%u hardware=0\n", calls); return 0;
}
