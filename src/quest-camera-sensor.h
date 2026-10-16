#ifndef REFRAMED_QUEST_CAMERA_SENSOR_H
#define REFRAMED_QUEST_CAMERA_SENSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QUEST_CAMERA_SENSOR_MAX_WRITES 4096

struct quest_camera_sensor_bus {
    uint32_t address_type, data_type, frequency;
    bool include_slave;
    uint32_t slave_address;
};

struct quest_camera_sensor_write {
    uint32_t address, value, delay_us;
};

/* Encode nonsecure Spectra random-write commands without submitting them.
 * Address/data types are byte widths 1..4; frequency uses the kernel enum 0..3.
 * An optional slave address uses the stock 8-bit address representation.
 * Delays below 2000us use hardware waits; longer waits must be whole milliseconds
 * and fit the kernel's signed 16-bit field. Inputs and output must not overlap.
 * Invalid inputs return zero and leave output unchanged. */
size_t quest_camera_sensor_writes_size(const struct quest_camera_sensor_bus *bus,
                                     const struct quest_camera_sensor_write *writes, size_t count);
size_t quest_camera_sensor_writes(const struct quest_camera_sensor_bus *bus,
                                const struct quest_camera_sensor_write *writes, size_t count,
                                void *output, size_t size);

struct quest_camera_sensor_probe {
    uint32_t register_address, expected_id, id_mask, camera_id, pipeline_delay;
};

/* Encode the 28-byte bus and sensor identity command. The slave address must
 * be present. Pipeline delay 0 keeps the kernel default; 1..3 selects frames.
 * The kernel reads this delay from the nominally reserved probe field.
 * V1 probe identity fields must fit 16 bits: the pinned kernel narrows them and
 * reads identity with word address/data widths. Encoded bus widths do not change
 * that read. This command does not select hardware or establish safe power settings. */
size_t quest_camera_sensor_probe(const struct quest_camera_sensor_bus *bus,
                               const struct quest_camera_sensor_probe *probe,
                               void *output, size_t size);

#define QUEST_CAMERA_SENSOR_MAX_POWER_STEPS 12

struct quest_camera_sensor_power_step {
    uint32_t sequence_type, value, delay_ms;
};

/* Power resources use the retained Quest kernel enum 0..12 (including VANA1).
 * A sequence has at most 12 steps; each command group stays below the 13-entry
 * enum limit required by its parser.
 * Values are board-specific; this encoder does not establish safe power levels.
 * Inputs and output must not overlap. Invalid inputs leave output unchanged. */
size_t quest_camera_sensor_power_size(const struct quest_camera_sensor_power_step *steps, size_t count);
size_t quest_camera_sensor_power(bool up, const struct quest_camera_sensor_power_step *steps,
                               size_t count, void *output, size_t size);

#endif
