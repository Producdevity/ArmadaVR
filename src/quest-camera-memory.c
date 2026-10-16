#include "quest-camera-memory.h"
#include <errno.h>
#include <string.h>
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

static bool valid_mmus(const int32_t *mmus, size_t count)
{
    if (count > 16 || (count && !mmus)) return false;
    for (size_t i = 0; i < count; ++i) {
        if (mmus[i] < 0) return false;
        for (size_t j = 0; j < i; ++j) if (mmus[i] == mmus[j]) return false;
    }
    return true;
}
int quest_camera_memory_allocate(int fd, enum quest_camera_memory_kind kind,
                                enum quest_camera_memory_mapping mapping, uint64_t size,
                                const int32_t *mmus, size_t count,
                                struct quest_camera_memory_allocation *allocation)
{
    if (!allocation || allocation->handle || allocation->fd != -1 || !size || size > INT32_MAX ||
        size % 4096 || kind < QUEST_CAMERA_MEMORY_PACKET || kind > QUEST_CAMERA_MEMORY_COMMAND ||
        mapping < QUEST_CAMERA_MEMORY_CPU || mapping > QUEST_CAMERA_MEMORY_SHARED ||
        !valid_mmus(mmus, count) || ((mapping == QUEST_CAMERA_MEMORY_CPU) != (count == 0)) ||
        (kind == QUEST_CAMERA_MEMORY_PACKET && mapping != QUEST_CAMERA_MEMORY_CPU)) return -EINVAL;
    struct {
        uint64_t length, alignment;
        int32_t mmus[16];
        uint32_t count, flags;
        struct { int32_t handle, fd; uint64_t address; } out;
    } data = {0};
    _Static_assert(sizeof(data) == 104, "CRM allocation ABI");
    data.length = size; data.alignment = 4096; data.count = (uint32_t)count;
    data.flags = 8 | 16 | 8192 | (kind == QUEST_CAMERA_MEMORY_PACKET ? 512 : 64);
    data.flags |= 1024;
    if (mapping != QUEST_CAMERA_MEMORY_CPU) data.flags |= 1;
    if (mapping == QUEST_CAMERA_MEMORY_SHARED) data.flags |= 2048;
    if (count) memcpy(data.mmus, mmus, count * sizeof(*mmus));
    data.out.fd = -1;
    int result = submit(fd, 0x116, &data, sizeof(data));
    if (data.out.handle != 0) allocation->handle = data.out.handle;
    if (data.out.fd >= 0) allocation->fd = data.out.fd;
    if (result) return result;
    if (data.out.handle <= 0 || data.out.fd < 0 || data.out.fd > 32767 || data.out.address ||
        ((uint32_t)data.out.handle >> 16) != (uint32_t)data.out.fd ||
        !(data.out.handle & 0x7fff) || (data.out.handle & 0x8000)) return -EPROTO;
    return 0;
}
int quest_camera_pixels_import(int fd, int dma_fd, const int32_t *mmus,
                              size_t count, uint32_t allocation_size, int32_t *handle)
{
    if (!allocation_size || allocation_size > INT32_MAX || allocation_size % 4096 ||
        dma_fd < 0 || dma_fd > 32767 || !count || !valid_mmus(mmus, count) || !handle || *handle) return -EINVAL;
    struct {
        int32_t mmus[16];
        uint32_t count, flags;
        int32_t fd;
        uint32_t reserved;
        struct { int32_t handle; uint32_t size; uint64_t address; } out;
    } data = {0};
    _Static_assert(sizeof(data) == 96, "CRM import ABI");
    memcpy(data.mmus, mmus, count * sizeof(*mmus));
    data.count = (uint32_t)count; data.flags = 1 | 16 | 128 | 8192 | 1024;
    data.fd = dma_fd;
    int result = submit(fd, 0x117, &data, sizeof(data));
    if (data.out.handle != 0) *handle = data.out.handle;
    if (result) return result;
    if (data.out.handle <= 0 || data.out.address || data.out.size != allocation_size ||
        ((uint32_t)data.out.handle >> 16) != (uint32_t)dma_fd ||
        !(data.out.handle & 0x7fff) || (data.out.handle & 0x8000)) return -EPROTO;
    return 0;
}
int quest_camera_memory_release(int fd, int32_t *handle)
{
    if (!handle || *handle <= 0) return -EINVAL;
    int32_t data[2] = {*handle, 0};
    int result = submit(fd, 0x118, data, sizeof(data));
    if (!result) *handle = 0;
    return result;
}

