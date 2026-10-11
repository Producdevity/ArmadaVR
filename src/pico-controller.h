#ifndef ARMADA_PICO_CONTROLLER_H
#define ARMADA_PICO_CONTROLLER_H

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

#endif
