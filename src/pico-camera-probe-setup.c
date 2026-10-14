#include "pico-camera-probe-setup.h"
#include "pico-camera-buffer.h"
#include "pico-camera-isp.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { IDENTITY, POWER, PACKET, BUFFERS };
struct pico_camera_probe_setup {
    int fd;
    struct pico_camera_buffer *buffers[BUFFERS];
    unsigned in_flight;
    bool prepared, submitted, quiesced;
};
int pico_camera_probe_setup_prepare(const struct pico_camera_probe_setup_config *c,
                                    struct pico_camera_probe_setup **setup)
{
    if (!c || !setup || *setup || c->crm_fd < 0 || c->sensor_fd < 0) return -EINVAL;
    uint8_t identity[28], power[2 * PICO_CAMERA_SENSOR_MAX_POWER_STEPS * 28];
    size_t up = pico_camera_sensor_power_size(c->power_up, c->power_up_count);
    size_t down = pico_camera_sensor_power_size(c->power_down, c->power_down_count);
    if (!up || !down || up + down > sizeof(power) ||
        !pico_camera_sensor_probe(&c->bus, &c->identity, identity, sizeof(identity)) ||
        pico_camera_sensor_power(true, c->power_up, c->power_up_count, power, sizeof(power)) != up ||
        pico_camera_sensor_power(false, c->power_down, c->power_down_count, power + up, sizeof(power) - up) != down)
        return -EINVAL;
    struct pico_camera_probe_setup *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    *setup = s; s->fd = c->sensor_fd;
    struct pico_camera_command_buffer commands[2] = {0};
    for (unsigned i = 0; i < BUFFERS; ++i) {
        struct pico_camera_buffer_config config = {
            .kind = i == PACKET ? PICO_CAMERA_BUFFER_PACKET : PICO_CAMERA_BUFFER_COMMAND,
            .mapping = PICO_CAMERA_MEMORY_CPU, .size = 4096, .cached = true};
        int result = pico_camera_buffer_create(c->crm_fd, -1, &config, &s->buffers[i]);
        if (result) return result;
        struct pico_camera_buffer_view view;
        if (!pico_camera_buffer_view(s->buffers[i], &view)) return -EINVAL;
        result = pico_camera_buffer_cpu_begin(s->buffers[i], ION_BUFFER_WRITE);
        if (result) return result;
        memset(view.data, 0, view.size);
        size_t size;
        if (i == PACKET) {
            struct pico_camera_packet_config packet = {.opcode = PICO_CAMERA_PACKET_SENSOR_PROBE,
                .commands = commands, .command_count = 2, .kernel_command_index = -1};
            size = pico_camera_packet_encode(&packet, view.data, view.size);
        } else {
            size = i == IDENTITY ? sizeof(identity) : up + down;
            memcpy(view.data, i == IDENTITY ? identity : power, size);
            commands[i] = (struct pico_camera_command_buffer){.memory_handle = view.memory_handle,
                .capacity = (uint32_t)view.size, .length = (uint32_t)size,
                .type = i == IDENTITY ? 10 : 7, .allocation_size = view.size};
        }
        result = pico_camera_buffer_cpu_end(s->buffers[i]);
        if (result || !size) return result ? result : -EINVAL;
    }
    s->prepared = true;
    return 0;
}
int pico_camera_probe_setup_submit(struct pico_camera_probe_setup *s)
{
    if (!s || !s->prepared || s->submitted) return -EINVAL;
    s->submitted = true;
    for (unsigned i = 0; i < BUFFERS; ++i) {
        int result = pico_camera_buffer_submit(s->buffers[i]);
        if (result) return result;
        s->in_flight |= 1U << i;
    }
    struct pico_camera_buffer_view packet;
    if (!pico_camera_buffer_view(s->buffers[PACKET], &packet)) return -EINVAL;
    return pico_camera_sensor_probe_submit(s->fd, packet.memory_handle);
}
int pico_camera_probe_setup_quiesced(struct pico_camera_probe_setup *s)
{
    if (!s || !s->submitted || s->quiesced) return -EINVAL;
    for (unsigned i = 0; i < BUFFERS; ++i) {
        if (!(s->in_flight & (1U << i))) continue;
        int result = pico_camera_buffer_complete(s->buffers[i]);
        if (result) return result;
        s->in_flight &= ~(1U << i);
    }
    s->quiesced = true;
    return 0;
}
int pico_camera_probe_setup_release(struct pico_camera_probe_setup **setup)
{
    if (!setup) return -EINVAL;
    struct pico_camera_probe_setup *s = *setup;
    if (!s) return 0;
    if (s->submitted && !s->quiesced) return -EBUSY;
    s->prepared = false;
    for (unsigned i = 0; i < BUFFERS; ++i) {
        int result = pico_camera_buffer_release(&s->buffers[i]);
        if (result) return result;
    }
    free(s); *setup = NULL;
    return 0;
}
