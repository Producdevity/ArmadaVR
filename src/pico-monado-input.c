#include "pico-monado-input.h"

#include <string.h>

enum {
    FIRST_CLICK, FIRST_TOUCH, SECOND_CLICK, SECOND_TOUCH, MENU, SYSTEM,
    TRIGGER_CLICK, TRIGGER_VALUE, TRIGGER_TOUCH, STICK_CLICK, STICK_TOUCH,
    STICK_VALUE, GRIP_CLICK, GRIP_VALUE, GRIP_POSE, AIM_POSE,
};

static bool valid_axis(const struct pico_axis_calibration *axis)
{
    return axis->minimum < axis->center && axis->center < axis->maximum &&
           axis->deadzone < axis->center - axis->minimum &&
           axis->deadzone < axis->maximum - axis->center;
}

bool pico_monado_input_begin(struct pico_monado_input *input, unsigned hand,
                             uint64_t generation, bool touch_supported,
                             const struct pico_analog_calibration *calibration)
{
    if (!input || hand < 1 || hand > 2 || !generation || (calibration &&
        (!valid_axis(&calibration->stick_x) || !valid_axis(&calibration->stick_y) ||
         calibration->trigger_released == calibration->trigger_pressed ||
         calibration->grip_released == calibration->grip_pressed)))
        return false;
    struct pico_monado_input next = {
        .generation = generation, .hand = hand, .touch_supported = touch_supported,
        .calibrated = calibration != NULL, .connected = true,
    };
    if (calibration)
        next.calibration = *calibration;
    const enum xrt_input_name names[PICO_MONADO_INPUT_COUNT] = {
        hand == 1 ? XRT_INPUT_PICO_NEO3_X_CLICK : XRT_INPUT_PICO_NEO3_A_CLICK,
        hand == 1 ? XRT_INPUT_PICO_NEO3_X_TOUCH : XRT_INPUT_PICO_NEO3_A_TOUCH,
        hand == 1 ? XRT_INPUT_PICO_NEO3_Y_CLICK : XRT_INPUT_PICO_NEO3_B_CLICK,
        hand == 1 ? XRT_INPUT_PICO_NEO3_Y_TOUCH : XRT_INPUT_PICO_NEO3_B_TOUCH,
        XRT_INPUT_PICO_NEO3_MENU_CLICK, XRT_INPUT_PICO_NEO3_SYSTEM_CLICK,
        XRT_INPUT_PICO_NEO3_TRIGGER_CLICK, XRT_INPUT_PICO_NEO3_TRIGGER_VALUE,
        XRT_INPUT_PICO_NEO3_TRIGGER_TOUCH, XRT_INPUT_PICO_NEO3_THUMBSTICK_CLICK,
        XRT_INPUT_PICO_NEO3_THUMBSTICK_TOUCH, XRT_INPUT_PICO_NEO3_THUMBSTICK,
        XRT_INPUT_PICO_NEO3_SQUEEZE_CLICK, XRT_INPUT_PICO_NEO3_SQUEEZE_VALUE,
        XRT_INPUT_PICO_NEO3_GRIP_POSE, XRT_INPUT_PICO_NEO3_AIM_POSE,
    };
    for (unsigned i = 0; i < PICO_MONADO_INPUT_COUNT; i++)
        next.inputs[i].name = names[i];
    *input = next;
    return true;
}

static float unit_value(uint8_t value, uint8_t released, uint8_t pressed)
{
    float normalized = ((int)value - released) / (float)((int)pressed - released);
    return normalized < 0 ? 0 : normalized > 1 ? 1 : normalized;
}

static float stick_value(uint8_t value, const struct pico_axis_calibration *axis)
{
    int low = axis->center - axis->deadzone, high = axis->center + axis->deadzone;
    float normalized = 0;
    if (value < low)
        normalized = -unit_value(value, low, axis->minimum);
    else if (value > high)
        normalized = unit_value(value, high, axis->maximum);
    return axis->inverted ? -normalized : normalized;
}

bool pico_monado_input_update(struct pico_monado_input *input,
                              const struct pico_controller_event *event)
{
    if (!input || !event || !input->connected || event->generation != input->generation ||
        event->sample.hand != input->hand || event->time.timestamp_ns < 0 ||
        (input->seen && event->time.timestamp_ns <= input->last_time_ns))
        return false;
    struct xrt_input *values = input->inputs;
    for (unsigned i = 0; i < PICO_MONADO_INPUT_COUNT; i++) {
        values[i].timestamp = event->time.timestamp_ns;
        values[i].active = i < GRIP_POSE;
        memset(&values[i].value, 0, sizeof(values[i].value));
    }
    uint8_t buttons = event->sample.buttons, touches = event->sample.touches;
    values[FIRST_CLICK].value.boolean = (buttons & PICO_BUTTON_A_X) != 0;
    values[SECOND_CLICK].value.boolean = (buttons & PICO_BUTTON_B_Y) != 0;
    values[MENU].value.boolean = (buttons & PICO_BUTTON_APP) != 0;
    values[SYSTEM].value.boolean = (buttons & PICO_BUTTON_HOME) != 0;
    values[TRIGGER_CLICK].value.boolean = (buttons & PICO_BUTTON_TRIGGER) != 0;
    values[STICK_CLICK].value.boolean = (buttons & PICO_BUTTON_STICK) != 0;
    values[GRIP_CLICK].value.boolean = (buttons & PICO_BUTTON_GRIP) != 0;
    values[FIRST_TOUCH].active = values[SECOND_TOUCH].active =
        values[TRIGGER_TOUCH].active = values[STICK_TOUCH].active = input->touch_supported;
    if (input->touch_supported) {
        values[FIRST_TOUCH].value.boolean = (touches & PICO_TOUCH_A_X) != 0;
        values[SECOND_TOUCH].value.boolean = (touches & PICO_TOUCH_B_Y) != 0;
        values[TRIGGER_TOUCH].value.boolean = (touches & PICO_TOUCH_TRIGGER) != 0;
        values[STICK_TOUCH].value.boolean = (touches & PICO_TOUCH_STICK) != 0;
    }
    values[TRIGGER_VALUE].active = values[GRIP_VALUE].active = values[STICK_VALUE].active = input->calibrated;
    if (input->calibrated) {
        const struct pico_analog_calibration *c = &input->calibration;
        values[TRIGGER_VALUE].value.vec1.x = unit_value(event->sample.trigger, c->trigger_released, c->trigger_pressed);
        values[GRIP_VALUE].value.vec1.x = unit_value(event->sample.grip, c->grip_released, c->grip_pressed);
        values[STICK_VALUE].value.vec2.x = stick_value(event->sample.stick_x, &c->stick_x);
        values[STICK_VALUE].value.vec2.y = stick_value(event->sample.stick_y, &c->stick_y);
    }
    input->last_time_ns = event->time.timestamp_ns;
    input->seen = true;
    return true;
}

bool pico_monado_input_disconnect(struct pico_monado_input *input, int64_t now_ns)
{
    if (!input || !input->generation || now_ns < 0 || now_ns < input->last_time_ns)
        return false;
    for (unsigned i = 0; i < PICO_MONADO_INPUT_COUNT; i++) {
        input->inputs[i].active = false;
        input->inputs[i].timestamp = now_ns;
        memset(&input->inputs[i].value, 0, sizeof(input->inputs[i].value));
    }
    input->last_time_ns = now_ns;
    input->connected = false;
    return true;
}
