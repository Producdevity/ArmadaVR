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
