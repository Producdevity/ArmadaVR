#ifndef REFRAMED_PICO_CAMERA_SENSOR_H
#define REFRAMED_PICO_CAMERA_SENSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_CAMERA_SENSOR_MAX_WRITES 4096

struct pico_camera_sensor_bus {
    uint32_t address_type, data_type, frequency;
    bool include_slave;
    uint32_t slave_address;
};

struct pico_camera_sensor_write {
    uint32_t address, value, delay_us;
};

/* Encode nonsecure Spectra random-write commands without submitting them.
 * Address/data types are byte widths 1..4; frequency uses the kernel enum 0..3.
 * An optional slave address uses the stock 8-bit address representation.
 * Delays below 2000us use hardware waits; longer waits must be whole milliseconds
 * and fit the kernel's signed 16-bit field. Inputs and output must not overlap.
 * Invalid inputs return zero and leave output unchanged. */
size_t pico_camera_sensor_writes_size(const struct pico_camera_sensor_bus *bus,
                                     const struct pico_camera_sensor_write *writes, size_t count);
size_t pico_camera_sensor_writes(const struct pico_camera_sensor_bus *bus,
                                const struct pico_camera_sensor_write *writes, size_t count,
                                void *output, size_t size);

#endif
