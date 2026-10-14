#include "pico-camera-sensor.h"

#include <string.h>

static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

size_t pico_camera_sensor_writes_size(const struct pico_camera_sensor_bus *bus,
                                     const struct pico_camera_sensor_write *writes, size_t count)
{
    if (!bus || !writes || !count || count > PICO_CAMERA_SENSOR_MAX_WRITES ||
        !bus->address_type || bus->address_type > 4 || !bus->data_type || bus->data_type > 4 ||
        bus->frequency > 3 || (bus->include_slave &&
        (!bus->slave_address || bus->slave_address > 254 || (bus->slave_address & 1)))) return 0;
    uint64_t address_limit = (uint64_t)1 << (bus->address_type * 8);
    uint64_t data_limit = (uint64_t)1 << (bus->data_type * 8);
    size_t size = bus->include_slave ? 8 : 0;
    bool new_group = true;
    for (size_t i = 0; i < count; ++i) {
        const struct pico_camera_sensor_write *w = writes + i;
        if (w->address >= address_limit || w->value >= data_limit ||
            w->delay_us > 32767000 || (w->delay_us >= 2000 && w->delay_us % 1000)) return 0;
        if (new_group) size += 8;
        size += 8;
        new_group = w->delay_us != 0;
        if (new_group) size += 8;
    }
    return size;
}

size_t pico_camera_sensor_writes(const struct pico_camera_sensor_bus *bus,
                                const struct pico_camera_sensor_write *writes, size_t count,
                                void *output, size_t size)
{
    size_t required = pico_camera_sensor_writes_size(bus, writes, count);
    if (!required || !output || size < required) return 0;
    uint8_t *bytes = output;
    memset(bytes, 0, required);
    size_t offset = 0, header = 0;
    uint32_t group_count = 0;
    if (bus->include_slave) {
        put32(bytes, bus->slave_address);
        bytes[4] = (uint8_t)bus->frequency;
        bytes[5] = 4;
        offset = 8;
    }
    for (size_t i = 0; i < count; ++i) {
        const struct pico_camera_sensor_write *w = writes + i;
        if (!group_count) {
            header = offset;
            bytes[offset + 4] = 1;
            bytes[offset + 5] = 5;
            bytes[offset + 6] = (uint8_t)bus->data_type;
            bytes[offset + 7] = (uint8_t)bus->address_type;
            offset += 8;
        }
        put32(bytes + offset, w->address);
        put32(bytes + offset + 4, w->value);
        offset += 8;
        put32(bytes + header, ++group_count);
        if (w->delay_us) {
            uint32_t delay = w->delay_us < 2000 ? w->delay_us : w->delay_us / 1000;
            bytes[offset] = (uint8_t)delay;
            bytes[offset + 1] = (uint8_t)(delay >> 8);
            bytes[offset + 4] = w->delay_us < 2000 ? 2 : 3;
            bytes[offset + 5] = 9;
            offset += 8;
            group_count = 0;
        }
    }
    return required;
}

size_t pico_camera_sensor_power_size(const struct pico_camera_sensor_power_step *steps, size_t count)
{
    if (!steps || !count || count > PICO_CAMERA_SENSOR_MAX_POWER_STEPS) return 0;
    size_t size = 0;
    unsigned group_count = 0;
    for (size_t i = 0; i < count; ++i) {
        if (steps[i].sequence_type > 11 || steps[i].delay_ms > 32767 || group_count == 11) return 0;
        if (!group_count++) size += 8;
        size += 12;
        if (steps[i].delay_ms) {
            size += 8;
            group_count = 0;
        }
    }
    return size;
}

size_t pico_camera_sensor_power(bool up, const struct pico_camera_sensor_power_step *steps,
                               size_t count, void *output, size_t size)
{
    size_t required = pico_camera_sensor_power_size(steps, count);
    if (!required || !output || size < required) return 0;
    uint8_t *bytes = output;
    memset(bytes, 0, required);
    size_t offset = 0, header = 0;
    uint32_t group_count = 0;
    for (size_t i = 0; i < count; ++i) {
        const struct pico_camera_sensor_power_step *step = steps + i;
        if (!group_count) {
            header = offset;
            bytes[offset + 5] = up ? 2 : 3;
            offset += 8;
        }
        bytes[offset] = (uint8_t)step->sequence_type;
        put32(bytes + offset + 4, step->value);
        offset += 12;
        put32(bytes + header, ++group_count);
        if (step->delay_ms) {
            bytes[offset] = (uint8_t)step->delay_ms;
            bytes[offset + 1] = (uint8_t)(step->delay_ms >> 8);
            bytes[offset + 4] = 3;
            bytes[offset + 5] = 9;
            offset += 8;
            group_count = 0;
        }
    }
    return required;
}

size_t pico_camera_sensor_probe(const struct pico_camera_sensor_bus *bus,
                               const struct pico_camera_sensor_probe *probe,
                               void *output, size_t size)
{
    if (!bus || !probe || !output || size < 28 || !bus->include_slave ||
        !bus->address_type || bus->address_type > 4 || !bus->data_type || bus->data_type > 4 ||
        bus->frequency > 3 || !bus->slave_address || bus->slave_address > 254 ||
        (bus->slave_address & 1) || probe->camera_id > UINT16_MAX || probe->pipeline_delay > 3 ||
        probe->register_address >= ((uint64_t)1 << (bus->address_type * 8))) return 0;
    uint8_t bytes[28] = {0};
    put32(bytes, bus->slave_address);
    bytes[4] = (uint8_t)bus->frequency;
    bytes[5] = 4;
    bytes[8] = (uint8_t)bus->data_type;
    bytes[9] = (uint8_t)bus->address_type;
    bytes[10] = 3;
    bytes[11] = 1;
    put32(bytes + 12, probe->register_address);
    put32(bytes + 16, probe->expected_id);
    put32(bytes + 20, probe->id_mask);
    bytes[24] = (uint8_t)probe->camera_id;
    bytes[25] = (uint8_t)(probe->camera_id >> 8);
    bytes[26] = (uint8_t)probe->pipeline_delay;
    memcpy(output, bytes, sizeof(bytes));
    return sizeof(bytes);
}
