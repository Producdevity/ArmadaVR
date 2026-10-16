#include "quest-camera-sensor-setup.h"
#include "quest-camera-control.h"
#include "quest-camera-packet.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct quest_camera_sensor_setup {
    int fd;
    int32_t session, sensor;
    struct { struct quest_camera_allocation *command, *packet; } stages[QUEST_CAMERA_SENSOR_STAGES];
    bool prepared, submitted, failed, quiesced, releasing;
    int release_error;
};
static const enum quest_camera_packet_opcode opcodes[QUEST_CAMERA_SENSOR_STAGES] = {
    QUEST_CAMERA_PACKET_SENSOR_INITIALIZE, QUEST_CAMERA_PACKET_SENSOR_CONFIGURE,
    QUEST_CAMERA_PACKET_SENSOR_CONFIGURE, QUEST_CAMERA_PACKET_SENSOR_STREAM_ON,
    QUEST_CAMERA_PACKET_SENSOR_STREAM_OFF,
};
static int prepare_stage(struct quest_camera_sensor_setup *s, unsigned stage, int crm,
                          const struct quest_camera_sensor_table *table, size_t bytes)
{
    struct quest_camera_allocation_config c = {.kind = QUEST_CAMERA_ALLOCATION_COMMAND,
        .mapping = QUEST_CAMERA_MEMORY_CPU, .size = (bytes + 4095) & ~(size_t)4095};
    int result = quest_camera_allocation_create(crm, -1, &c, &s->stages[stage].command);
    if (result) return result;
    c.kind = QUEST_CAMERA_ALLOCATION_PACKET; c.size = 4096;
    result = quest_camera_allocation_create(crm, -1, &c, &s->stages[stage].packet);
    if (result) return result;
    struct quest_camera_allocation_view command, packet;
    if (!quest_camera_allocation_view(s->stages[stage].command, &command) ||
        !quest_camera_allocation_view(s->stages[stage].packet, &packet)) return -EINVAL;
    result = quest_camera_allocation_cpu_begin(s->stages[stage].command, QUEST_CAMERA_CPU_WRITE);
    if (result) return result;
    memset(command.data, 0, command.size);
    size_t encoded = quest_camera_sensor_writes(&table->bus, table->writes, table->count, command.data, command.size);
    result = quest_camera_allocation_cpu_end(s->stages[stage].command);
    if (result || encoded != bytes) return result ? result : -EINVAL;
    struct quest_camera_command_buffer reference = {.memory_handle = command.handle,
        .capacity = (uint32_t)command.size, .length = (uint32_t)bytes, .type = 7,
        .allocation_size = command.size};
    struct quest_camera_packet_config config = {.opcode = opcodes[stage], .commands = &reference,
        .command_count = 1, .kernel_command_index = -1};
    result = quest_camera_allocation_cpu_begin(s->stages[stage].packet, QUEST_CAMERA_CPU_WRITE);
    if (result) return result;
    memset(packet.data, 0, packet.size);
    encoded = quest_camera_packet_encode(&config, packet.data, packet.size);
    result = quest_camera_allocation_cpu_end(s->stages[stage].packet);
    return result ? result : encoded ? 0 : -EINVAL;
}
int quest_camera_sensor_setup_prepare(const struct quest_camera_sensor_setup_config *c,
                                      struct quest_camera_sensor_setup **setup)
{
    if (!c || !setup || *setup || c->crm_fd < 0 || c->sensor_fd < 0 || c->crm_fd == c->sensor_fd ||
        c->session <= 0 || c->sensor <= 0) return -EINVAL;
    const struct quest_camera_sensor_table *tables[QUEST_CAMERA_SENSOR_STAGES] = {
        &c->initialize, &c->mode, &c->synchronize, &c->stream_on, &c->stream_off,
    };
    size_t sizes[QUEST_CAMERA_SENSOR_STAGES] = {0};
    for (unsigned i = 0; i < QUEST_CAMERA_SENSOR_STAGES; ++i) {
        if ((i == QUEST_CAMERA_SENSOR_MODE || i == QUEST_CAMERA_SENSOR_SYNCHRONIZE) && !tables[i]->count) continue;
        sizes[i] = quest_camera_sensor_writes_size(&tables[i]->bus, tables[i]->writes, tables[i]->count);
        if (!sizes[i]) return -EINVAL;
    }
    struct quest_camera_sensor_setup *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    *setup = s; s->fd = c->sensor_fd; s->session = c->session; s->sensor = c->sensor;
    for (unsigned i = 0; i < QUEST_CAMERA_SENSOR_STAGES; ++i) {
        if (!sizes[i]) continue;
        int result = prepare_stage(s, i, c->crm_fd, tables[i], sizes[i]);
        if (result) { s->failed = true; return result; }
    }
    s->prepared = true;
    return 0;
}
int quest_camera_sensor_setup_submit(struct quest_camera_sensor_setup *s)
{
    if (!s || !s->prepared || s->submitted || s->failed || s->quiesced || s->releasing) return -EINVAL;
    s->submitted = true;
    for (unsigned i = 0; i < QUEST_CAMERA_SENSOR_STAGES; ++i) {
        if (!s->stages[i].packet) continue;
        struct quest_camera_allocation_view packet;
        if (!quest_camera_allocation_view(s->stages[i].packet, &packet)) return -EINVAL;
        int result = quest_camera_allocation_submit(s->stages[i].command);
        if (!result) result = quest_camera_allocation_submit(s->stages[i].packet);
        if (!result) result = quest_camera_device_configure(s->fd, s->session, s->sensor, packet.handle, 0, packet.size);
        if (result) return result;
    }
    return 0;
}
int quest_camera_sensor_setup_quiesced(struct quest_camera_sensor_setup *s)
{
    if (!s || s->quiesced || s->releasing) return -EINVAL;
    for (unsigned i = 0; i < QUEST_CAMERA_SENSOR_STAGES; ++i) {
        if (s->stages[i].command) {
            int result = quest_camera_allocation_quiesced(s->stages[i].command);
            if (result) return result;
        }
        if (s->stages[i].packet) {
            int result = quest_camera_allocation_quiesced(s->stages[i].packet);
            if (result) return result;
        }
    }
    s->quiesced = true; s->prepared = false;
    return 0;
}
int quest_camera_sensor_setup_release(struct quest_camera_sensor_setup **setup)
{
    if (!setup) return -EINVAL;
    struct quest_camera_sensor_setup *s = *setup;
    if (!s) return 0;
    if ((s->submitted || s->failed) && !s->quiesced) return -EBUSY;
    if (s->releasing) return s->release_error;
    s->releasing = true; s->prepared = false;
    for (unsigned i = 0; i < QUEST_CAMERA_SENSOR_STAGES; ++i) {
        int result = quest_camera_allocation_release(&s->stages[i].packet);
        if (result && !s->release_error) s->release_error = result;
        result = quest_camera_allocation_release(&s->stages[i].command);
        if (result && !s->release_error) s->release_error = result;
    }
    if (s->release_error) return s->release_error;
    free(s); *setup = NULL;
    return 0;
}
bool quest_camera_sensor_setup_resources(const struct quest_camera_sensor_setup *s,
    enum quest_camera_sensor_stage stage, bool packet, struct quest_camera_allocation_view *view)
{
    if (!s || stage < 0 || stage >= QUEST_CAMERA_SENSOR_STAGES) return false;
    return quest_camera_allocation_resources(packet ? s->stages[stage].packet : s->stages[stage].command, view);
}
void quest_camera_sensor_setup_closed(struct quest_camera_sensor_setup **setup)
{
    if (!setup || !*setup) return;
    struct quest_camera_sensor_setup *s = *setup;
    for (unsigned i = 0; i < QUEST_CAMERA_SENSOR_STAGES; ++i) {
        quest_camera_allocation_closed(&s->stages[i].packet);
        quest_camera_allocation_closed(&s->stages[i].command);
    }
    free(s); *setup = NULL;
}
