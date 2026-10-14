#ifndef REFRAMED_PICO_CONTROLLER_H
#define REFRAMED_PICO_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum pico_controller_button {
    PICO_BUTTON_HOME = 1,
    PICO_BUTTON_APP = 2,
    PICO_BUTTON_A_X = 4,
    PICO_BUTTON_B_Y = 8,
    PICO_BUTTON_GRIP = 16,
    PICO_BUTTON_STICK = 32,
    PICO_BUTTON_TRIGGER = 128,
};

enum pico_controller_touch {
    PICO_TOUCH_A_X = 1,
    PICO_TOUCH_B_Y = 2,
    PICO_TOUCH_STICK = 4,
    PICO_TOUCH_THUMBREST = 8,
    PICO_TOUCH_TRIGGER = 16,
};

struct pico_controller_sample {
    uint8_t hand;
    uint8_t buttons;
    uint8_t touches;
    uint8_t trigger;
    uint8_t grip;
    uint8_t stick_x;
    uint8_t stick_y;
    uint8_t battery_level;
};

/* Decode a complete 100-byte station SLPI record from Neo3 b1977.
 * MCU type and radio type are explicit stock configuration values (1 or 2).
 * Hand is 1=left, 2=right; analog controls retain their unsigned wire values.
 * Radio type 1 converts percentages to levels 1..5; type 2 retains the low
 * three battery bits. No service key masks or analog calibration are applied.
 * Failure, including a valid record without key data, leaves sample unchanged. */
bool pico_controller_decode(const void *record, size_t length, unsigned mcu_type,
                            unsigned radio_type, bool touch_supported,
                            struct pico_controller_sample *sample);

/* Decode the sns_ble_mcu_data payload of an SSC 0x400 event, not its QMI or
 * sns_client_event_msg envelope. Accept packed/unpacked fixed32 words and
 * accuracy 0..3; require exactly 25 words. Unknown scalar fields are skipped;
 * groups and payloads above 4096 bytes are refused. Both outputs are unchanged
 * on failure. Stream identity, timestamps and connection lifetime belong to
 * the caller; no pointer into the payload is retained. */
bool pico_controller_decode_ssc(const void *payload, size_t length,
                                unsigned mcu_type, unsigned radio_type,
                                bool touch_supported,
                                struct pico_controller_sample *sample,
                                unsigned *accuracy);

struct pico_controller_imu_sample {
    uint64_t timestamp_us;
    int16_t gyroscope[3], acceleration[3], temperature;
    uint8_t hand, sequence;
};

struct pico_controller_imu_batch {
    uint64_t station_time;
    struct pico_controller_imu_sample samples[2];
    unsigned count;
};

/* Decode raw IMU counts from a complete b1977 station record. connected_mask
 * uses bit 0 for left and bit 1 for right; MCU/radio types must be explicit.
 * Samples retain wire order. A valid packet may contain no nonzero timestamps.
 * Controller timestamps are microseconds, but their host-clock mapping is not
 * established. Station time has a separate, unresolved clock. No vector units, bias,
 * hardware axis reversal, freshness or pose are inferred; failure preserves batch. */
bool pico_controller_decode_imu(const void *record, size_t length, unsigned mcu_type,
                                unsigned radio_type, unsigned connected_mask,
                                struct pico_controller_imu_batch *batch);

/* Decode the same 0x400 SSC payload accepted by pico_controller_decode_ssc.
 * Both outputs are unchanged on failure. Accuracy does not establish calibration. */
bool pico_controller_decode_imu_ssc(const void *payload, size_t length, unsigned mcu_type,
                                    unsigned radio_type, unsigned connected_mask,
                                    struct pico_controller_imu_batch *batch, unsigned *accuracy);

#endif
