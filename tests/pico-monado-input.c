#include "pico-monado-input.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static struct xrt_input *named(struct pico_monado_input *input, enum xrt_input_name name)
{
    for (unsigned i = 0; i < PICO_MONADO_INPUT_COUNT; i++)
        if (input->inputs[i].name == name)
            return &input->inputs[i];
    assert(false);
    return NULL;
}

static void near(float actual, float expected)
{
    assert(isfinite(actual) && fabsf(actual - expected) < 0.00001f);
}

int main(void)
{
    struct pico_analog_calibration calibration = {
        .stick_x = {10, 120, 230, 10, false},
        .stick_y = {20, 128, 240, 8, true},
        .trigger_released = 10, .trigger_pressed = 210,
        .grip_released = 240, .grip_pressed = 40,
    };
    struct pico_monado_input input;
    struct pico_controller_event event = {.generation = 8, .time.timestamp_ns = 100};
    unsigned checks = 0;
    for (unsigned hand = 1; hand <= 2; hand++) {
        assert(pico_monado_input_begin(&input, hand, 8, true, &calibration));
        event.sample.hand = hand;
        event.time.timestamp_ns = 100;
        enum xrt_input_name names[] = {
            XRT_INPUT_PICO_NEO3_SYSTEM_CLICK, XRT_INPUT_PICO_NEO3_MENU_CLICK,
            hand == 1 ? XRT_INPUT_PICO_NEO3_X_CLICK : XRT_INPUT_PICO_NEO3_A_CLICK,
            hand == 1 ? XRT_INPUT_PICO_NEO3_Y_CLICK : XRT_INPUT_PICO_NEO3_B_CLICK,
            XRT_INPUT_PICO_NEO3_SQUEEZE_CLICK, XRT_INPUT_PICO_NEO3_THUMBSTICK_CLICK,
            XRT_INPUT_PICO_NEO3_TRIGGER_CLICK,
        };
        unsigned masks[] = {1, 2, 4, 8, 16, 32, 128};
        for (unsigned buttons = 0; buttons < 256; buttons++) {
            event.sample.buttons = buttons;
            event.time.timestamp_ns++;
            assert(pico_monado_input_update(&input, &event));
            for (unsigned i = 0; i < 7; i++) {
                struct xrt_input *value = named(&input, names[i]);
                assert(value->active && value->value.boolean == ((buttons & masks[i]) != 0));
                assert(value->timestamp == event.time.timestamp_ns);
                checks++;
            }
        }
        enum xrt_input_name touches[] = {
            hand == 1 ? XRT_INPUT_PICO_NEO3_X_TOUCH : XRT_INPUT_PICO_NEO3_A_TOUCH,
            hand == 1 ? XRT_INPUT_PICO_NEO3_Y_TOUCH : XRT_INPUT_PICO_NEO3_B_TOUCH,
            XRT_INPUT_PICO_NEO3_THUMBSTICK_TOUCH, XRT_INPUT_PICO_NEO3_TRIGGER_TOUCH,
        };
        unsigned touch_masks[] = {1, 2, 4, 16};
        for (unsigned bits = 0; bits < 32; bits++) {
            event.sample.touches = bits;
            event.time.timestamp_ns++;
            assert(pico_monado_input_update(&input, &event));
            for (unsigned i = 0; i < 4; i++) {
                assert(named(&input, touches[i])->value.boolean == ((bits & touch_masks[i]) != 0));
                checks++;
            }
        }
        for (unsigned raw = 0; raw < 256; raw++) {
            event.sample.trigger = event.sample.grip = event.sample.stick_x = event.sample.stick_y = raw;
            event.time.timestamp_ns++;
            assert(pico_monado_input_update(&input, &event));
            float trigger = named(&input, XRT_INPUT_PICO_NEO3_TRIGGER_VALUE)->value.vec1.x;
            float grip = named(&input, XRT_INPUT_PICO_NEO3_SQUEEZE_VALUE)->value.vec1.x;
            struct xrt_vec2 stick = named(&input, XRT_INPUT_PICO_NEO3_THUMBSTICK)->value.vec2;
            near(trigger, raw <= 10 ? 0 : raw >= 210 ? 1 : (raw - 10) / 200.0f);
            near(grip, raw <= 40 ? 1 : raw >= 240 ? 0 : (240 - raw) / 200.0f);
            near(stick.x, raw <= 10 ? -1 : raw >= 230 ? 1 :
                 raw < 110 ? -(float)(110 - raw) / 100.0f : raw > 130 ? (raw - 130) / 100.0f : 0);
            near(stick.y, raw <= 20 ? 1 : raw >= 240 ? -1 :
                 raw < 120 ? (120 - raw) / 100.0f : raw > 136 ? -(float)(raw - 136) / 104.0f : 0);
            assert(!named(&input, XRT_INPUT_PICO_NEO3_GRIP_POSE)->active);
            assert(!named(&input, XRT_INPUT_PICO_NEO3_AIM_POSE)->active);
            checks += 6;
        }
        unsigned char saved[sizeof(input)];
        memcpy(saved, &input, sizeof(input));
        assert(!pico_monado_input_update(&input, &event));
        assert(!memcmp(saved, &input, sizeof(input)));
        event.time.timestamp_ns++;
        event.sample.hand = 3 - hand;
        assert(!pico_monado_input_update(&input, &event));
        assert(!memcmp(saved, &input, sizeof(input)));
        event.sample.hand = hand;
        event.generation = 7;
        assert(!pico_monado_input_update(&input, &event));
        assert(!memcmp(saved, &input, sizeof(input)));
        event.generation = 8;
        assert(!pico_monado_input_disconnect(&input, input.last_time_ns - 1));
        assert(!memcmp(saved, &input, sizeof(input)));
        assert(pico_monado_input_disconnect(&input, event.time.timestamp_ns));
        for (unsigned i = 0; i < PICO_MONADO_INPUT_COUNT; i++) {
            const unsigned char zero[sizeof(input.inputs[i].value)] = {0};
            assert(!input.inputs[i].active && !memcmp(&input.inputs[i].value, zero, sizeof(zero)));
        }
        assert(!pico_monado_input_update(&input, &event));
        assert(pico_monado_input_begin(&input, hand, 9, false, NULL));
        event.generation = 9;
        assert(pico_monado_input_update(&input, &event));
        for (unsigned i = 0; i < 4; i++)
            assert(!named(&input, touches[i])->active);
        assert(!named(&input, XRT_INPUT_PICO_NEO3_TRIGGER_VALUE)->active);
        assert(!named(&input, XRT_INPUT_PICO_NEO3_THUMBSTICK)->active);
        assert(!named(&input, XRT_INPUT_PICO_NEO3_SQUEEZE_VALUE)->active);
        event.generation = 8;
    }
    unsigned char saved[sizeof(input)];
    memcpy(saved, &input, sizeof(input));
    calibration.stick_x.deadzone = 110;
    assert(!pico_monado_input_begin(&input, 1, 10, true, &calibration));
    assert(!memcmp(saved, &input, sizeof(input)));
    calibration.stick_x.deadzone = 10;
    calibration.grip_pressed = calibration.grip_released;
    assert(!pico_monado_input_begin(&input, 1, 10, true, &calibration));
    assert(!memcmp(saved, &input, sizeof(input)));
    assert(!pico_monado_input_begin(&input, 0, 10, true, NULL));
    assert(!pico_monado_input_begin(&input, 1, 0, true, NULL));
    assert(!pico_monado_input_begin(NULL, 1, 10, true, NULL));
    assert(!pico_monado_input_update(NULL, &event));
    assert(!pico_monado_input_update(&input, NULL));
    assert(!pico_monado_input_disconnect(NULL, 0));
    printf("PICO_MONADO_INPUT_PASS checks=%u\n", checks);
    return 0;
}
