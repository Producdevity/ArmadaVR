#include "pico-camera-request.h"
#include "pico-camera-isp.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { PIXELS, SENSOR_PACKET, IFE_PACKET, IFE_COMMAND, RESOURCE_COUNT };
struct pico_camera_request {
    struct pico_camera_request_config config;
    struct pico_camera_buffer *buffers[RESOURCE_COUNT];
    int32_t fence;
    unsigned in_flight;
    bool prepared, submitted, completed, fence_release_failed;
};
bool pico_camera_request_config_valid(const struct pico_camera_request_config *c)
{
    return c && c->crm_fd >= 0 && c->sync_fd >= 0 && c->ion_fd >= 0 &&
        c->sensor_fd >= 0 && c->ife_fd >= 0 && c->session > 0 && c->link > 0 &&
        c->sensor > 0 && c->ife > 0 && c->sensor != c->ife &&
        c->device_mmu >= 0 && c->command_mmu >= 0 && c->ion_heap < 32 &&
        (c->command_mapping == PICO_CAMERA_MEMORY_IO || c->command_mapping == PICO_CAMERA_MEMORY_SHARED) &&
        c->width && c->width <= 4096 && c->height && c->height <= 4096 &&
        c->stride >= c->width && c->stride <= 8192 && !(c->stride % 16) &&
        c->generation && c->request_id && c->request_id <= INT64_MAX;
}
static int write_packet(struct pico_camera_buffer *buffer, const struct pico_camera_packet_config *config)
{
    struct pico_camera_buffer_view view;
    if (!pico_camera_buffer_view(buffer, &view)) return -EINVAL;
    int result = pico_camera_buffer_cpu_begin(buffer, ION_BUFFER_WRITE);
    if (result) return result;
    memset(view.data, 0, view.size);
    bool encoded = pico_camera_packet_encode(config, view.data, view.size) != 0;
    result = pico_camera_buffer_cpu_end(buffer);
    return result ? result : encoded ? 0 : -EINVAL;
}
int pico_camera_request_prepare(const struct pico_camera_request_config *config,
                                struct pico_camera_request **request)
{
    if (!request || *request || !pico_camera_request_config_valid(config)) return -EINVAL;
    struct pico_camera_request *r = calloc(1, sizeof(*r));
    if (!r) return -ENOMEM;
    *request = r; r->config = *config;
    struct pico_camera_buffer_config allocation = {.kind = PICO_CAMERA_BUFFER_PIXELS,
        .mapping = PICO_CAMERA_MEMORY_IO,
        .size = ((size_t)config->stride * config->height + 4095) & ~(size_t)4095,
        .mmus = {config->device_mmu}, .mmu_count = 1, .ion_heap = config->ion_heap, .cached = true};
    int result = pico_camera_buffer_create(config->crm_fd, config->ion_fd, &allocation, &r->buffers[PIXELS]);
    if (result) return result;
    allocation = (struct pico_camera_buffer_config){.kind = PICO_CAMERA_BUFFER_PACKET,
        .mapping = PICO_CAMERA_MEMORY_CPU, .size = 4096, .cached = true};
    for (unsigned i = SENSOR_PACKET; i <= IFE_PACKET; ++i) {
        result = pico_camera_buffer_create(config->crm_fd, config->ion_fd, &allocation, &r->buffers[i]);
        if (result) return result;
    }
    allocation.kind = PICO_CAMERA_BUFFER_COMMAND; allocation.mapping = config->command_mapping;
    allocation.mmus[0] = config->command_mmu; allocation.mmu_count = 1;
    result = pico_camera_buffer_create(config->crm_fd, config->ion_fd, &allocation, &r->buffers[IFE_COMMAND]);
    if (result) return result;
    result = pico_camera_fence_create(config->sync_fd, "pico-camera", &r->fence);
    if (result) return result;
    result = pico_camera_fence_register(config->sync_fd, r->fence, config->generation, config->request_id);
    if (result) return result;
    struct pico_camera_buffer_view image, command;
    if (!pico_camera_buffer_view(r->buffers[PIXELS], &image) ||
        !pico_camera_buffer_view(r->buffers[IFE_COMMAND], &command)) return -EINVAL;
    result = pico_camera_buffer_cpu_begin(r->buffers[IFE_COMMAND], ION_BUFFER_WRITE);
    if (result) return result;
    memset(command.data, 0, command.size);
    result = pico_camera_buffer_cpu_end(r->buffers[IFE_COMMAND]);
    if (result) return result;
    struct pico_camera_packet_config packet = {.opcode = PICO_CAMERA_PACKET_SENSOR_NOP,
        .request_id = config->request_id, .kernel_command_index = -1};
    result = write_packet(r->buffers[SENSOR_PACKET], &packet);
    if (result) return result;
    struct pico_camera_command_buffer kmd = {.memory_handle = command.memory_handle,
        .capacity = (uint32_t)command.size, .type = 5, .metadata = 3, .allocation_size = command.size};
    struct pico_camera_rdi_buffer rdi = {.memory_handle = image.memory_handle,
        .completion_fence = r->fence, .width = config->width, .height = config->height,
        .stride = config->stride, .slice_height = config->height, .bits = 8,
        .format = PICO_CAMERA_RAW_MIPI, .allocation_size = image.size};
    packet = (struct pico_camera_packet_config){.opcode = PICO_CAMERA_PACKET_IFE_UPDATE,
        .request_id = config->request_id, .commands = &kmd, .command_count = 1,
        .kernel_command_index = 0, .image = &rdi};
    result = write_packet(r->buffers[IFE_PACKET], &packet);
    if (!result) r->prepared = true;
    return result;
}
bool pico_camera_request_capture(const struct pico_camera_request *r, struct pico_camera_capture_buffer *buffer)
{
    struct pico_camera_buffer_view view;
    if (!r || !buffer || !r->prepared || !pico_camera_buffer_view(r->buffers[PIXELS], &view)) return false;
    *buffer = (struct pico_camera_capture_buffer){.token = (void *)r, .request_id = r->config.request_id,
        .memory_handle = view.memory_handle, .fence = r->fence, .camera_id = r->config.camera_id,
        .width = r->config.width, .height = r->config.height, .stride = r->config.stride,
        .pixels = view.data, .size = view.size};
    return true;
}
int pico_camera_request_submit(struct pico_camera_request *r)
{
    if (!r || !r->prepared || r->submitted) return -EINVAL;
    struct pico_camera_buffer_view sensor, ife;
    if (!pico_camera_buffer_view(r->buffers[SENSOR_PACKET], &sensor) ||
        !pico_camera_buffer_view(r->buffers[IFE_PACKET], &ife)) return -EINVAL;
    r->submitted = true;
    for (unsigned i = 0; i < RESOURCE_COUNT; ++i) {
        int result = pico_camera_buffer_submit(r->buffers[i]);
        if (result) return result;
        r->in_flight |= 1U << i;
    }
    const struct pico_camera_request_config *c = &r->config;
    int result = pico_camera_request_schedule(c->crm_fd, c->session, c->link, c->request_id, c->synchronized, false);
    if (!result) result = pico_camera_device_configure(c->sensor_fd, c->session, c->sensor,
        sensor.memory_handle, 0, sensor.size);
    if (!result) result = pico_camera_device_configure(c->ife_fd, c->session, c->ife,
        ife.memory_handle, 0, ife.size);
    return result;
}
int pico_camera_request_complete(struct pico_camera_request *r)
{
    if (!r || !r->submitted || r->completed) return -EINVAL;
    for (unsigned i = 0; i < RESOURCE_COUNT; ++i) {
        if (!(r->in_flight & (1U << i))) continue;
        int result = pico_camera_buffer_complete(r->buffers[i]);
        if (result) return result;
        r->in_flight &= ~(1U << i);
    }
    r->completed = true;
    return 0;
}
int pico_camera_request_cpu_sync(struct pico_camera_request *r, bool end)
{
    if (!r || !r->completed) return -EINVAL;
    return end ? pico_camera_buffer_cpu_end(r->buffers[PIXELS]) :
                 pico_camera_buffer_cpu_begin(r->buffers[PIXELS], ION_BUFFER_READ);
}
int pico_camera_request_release(struct pico_camera_request **request)
{
    if (!request) return -EINVAL;
    struct pico_camera_request *r = *request;
    if (!r) return 0;
    if (r->submitted && !r->completed) return -EBUSY;
    if (r->fence_release_failed) return -EIO;
    r->prepared = false;
    if (r->fence > 0) {
        int result = pico_camera_fence_destroy(r->config.sync_fd, &r->fence);
        if (result) { r->fence_release_failed = true; return result; }
    }
    for (unsigned i = 0; i < RESOURCE_COUNT; ++i) {
        int result = pico_camera_buffer_release(&r->buffers[i]);
        if (result) return result;
    }
    free(r); *request = NULL;
    return 0;
}
