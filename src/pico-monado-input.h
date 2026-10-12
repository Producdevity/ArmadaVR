#ifndef ARMADA_PICO_MONADO_INPUT_H
#define ARMADA_PICO_MONADO_INPUT_H

#include "pico-controller-stream.h"
#include <xrt/xrt_device.h>

#define PICO_MONADO_INPUT_COUNT 16

struct pico_axis_calibration {
    uint8_t minimum, center, maximum, deadzone;
    bool inverted;
};

struct pico_analog_calibration {
    struct pico_axis_calibration stick_x, stick_y;
    uint8_t trigger_released, trigger_pressed, grip_released, grip_pressed;
};

struct pico_monado_input {
    struct xrt_input inputs[PICO_MONADO_INPUT_COUNT];
    struct pico_analog_calibration calibration;
    uint64_t generation;
    int64_t last_time_ns;
    unsigned hand;
    bool calibrated, touch_supported, connected, seen;
};

/* Use from the owning runtime's input thread. Calibration is explicit; NULL
 * exposes digital controls while keeping analog inputs inactive. Grip/aim
 * pose inputs remain inactive until a separate tracking implementation exists. */
bool pico_monado_input_begin(struct pico_monado_input *input, unsigned hand,
                             uint64_t generation, bool touch_supported,
                             const struct pico_analog_calibration *calibration);

/* Consume an event accepted by pico_controller_stream_report. Reject another
 * hand, old subscription or reversed timestamp without changing input state. */
bool pico_monado_input_update(struct pico_monado_input *input,
                              const struct pico_controller_event *event);

/* Clear all values and deactivate inputs on disconnect or stream timeout.
 * now_ns must use the same host-clock domain as the accepted events. */
bool pico_monado_input_disconnect(struct pico_monado_input *input, int64_t now_ns);

#endif
