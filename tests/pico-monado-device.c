#include "pico-monado-device.h"

#include <os/os_time.h>
#include <xrt/xrt_tracking.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const struct pico_analog_calibration calibration = {
    .stick_x = {0, 128, 255, 0, false}, .stick_y = {0, 128, 255, 0, false},
    .trigger_pressed = 255, .grip_pressed = 255,
};

static struct pico_controller_event event(unsigned hand, uint64_t generation, bool pressed)
{
    return (struct pico_controller_event){
        .sample = {.hand = hand, .buttons = pressed ? PICO_BUTTON_A_X : 0,
                   .trigger = pressed ? 255 : 0, .stick_x = 128, .stick_y = 128},
        .time.timestamp_ns = (int64_t)os_monotonic_get_ns(), .generation = generation,
    };
}

static void inactive(struct xrt_device *device)
{
    assert(xrt_device_update_inputs(device) == XRT_SUCCESS);
    union xrt_input_value zero = {0};
    for (unsigned i = 0; i < device->input_count; i++) {
        assert(!device->inputs[i].active);
        assert(!memcmp(&device->inputs[i].value, &zero, sizeof(zero)));
    }
}

static void inspect(struct xrt_device *device, bool expected_active, bool pressed)
{
    assert(xrt_device_update_inputs(device) == XRT_SUCCESS);
    unsigned found = 0;
    for (unsigned i = 0; i < device->input_count; i++) {
        struct xrt_input *input = &device->inputs[i];
        if (input->name == XRT_INPUT_PICO_NEO3_A_CLICK || input->name == XRT_INPUT_PICO_NEO3_X_CLICK) {
            assert(input->active == expected_active);
            assert(input->value.boolean == pressed);
            found++;
        }
        if (input->name == XRT_INPUT_PICO_NEO3_TRIGGER_VALUE) {
            assert(input->active == expected_active);
            assert(input->value.vec1.x == (pressed ? 1.0f : 0.0f));
            found++;
        }
    }
    assert(found == 2);
}

static void lifecycle(void)
{
    struct pico_monado_producer *producer = NULL;
    struct xrt_device *left = NULL, *right = NULL;
    assert(!pico_monado_devices_create(0, &producer, &left, &right));
    assert(!pico_monado_devices_create(1, &producer, &left, &left));
    assert(!producer && !left && !right);
    assert(pico_monado_devices_create(1000000000, &producer, &left, &right));
    assert(!pico_monado_devices_create(1, &producer, &left, &right));
    assert(left->id.val && right->id.val && left->id.val != right->id.val);
    assert(left->name == XRT_DEVICE_PICO_NEO3_CONTROLLER && right->name == left->name);
    assert(left->device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER);
    assert(right->device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER);
    assert(!left->supported.orientation_tracking && !left->supported.position_tracking);
    assert(!left->output_count && !right->output_count);
    assert(left->tracking_origin->type == XRT_TRACKING_TYPE_NONE);
    inactive(left);
    inactive(right);
    struct pico_controller_event sample = event(1, 1, true);
    assert(!pico_monado_producer_push(producer, &sample));
    assert(pico_monado_producer_begin(producer, 1, false, &calibration, &calibration));
    assert(pico_monado_producer_push(producer, &sample));
    inspect(left, true, true);
    inactive(right);
    assert(!pico_monado_producer_push(producer, &sample));
    sample = event(2, 1, true);
    assert(pico_monado_producer_push(producer, &sample));
    inspect(right, true, true);
    sample = event(1, 1, false);
    sample.time.timestamp_ns = INT64_MAX;
    assert(!pico_monado_producer_push(producer, &sample));
    sample.time.timestamp_ns = -1;
    assert(!pico_monado_producer_push(producer, &sample));
    sample.time.timestamp_ns = (int64_t)os_monotonic_get_ns() - 2000000000;
    assert(!pico_monado_producer_push(producer, &sample));
    sample = event(3, 1, false);
    assert(!pico_monado_producer_push(producer, &sample));
    struct pico_analog_calibration invalid = calibration;
    invalid.stick_x.center = 0;
    assert(!pico_monado_producer_begin(producer, 2, false, &calibration, &invalid));
    inspect(left, true, true);
    assert(!pico_monado_producer_begin(producer, 1, false, NULL, NULL));
    struct xrt_space_relation relation;
    memset(&relation, 0xff, sizeof(relation));
    assert(xrt_device_get_tracked_pose(left, XRT_INPUT_PICO_NEO3_GRIP_POSE, 0, &relation) == XRT_SUCCESS);
    assert(relation.relation_flags == 0 && relation.pose.orientation.w == 1);
    assert(relation.pose.position.x == 0 && relation.linear_velocity.x == 0);
    assert(xrt_device_get_tracked_pose(right, XRT_INPUT_PICO_NEO3_AIM_POSE, 0, &relation) == XRT_SUCCESS);
    assert(relation.relation_flags == 0);
    assert(xrt_device_get_tracked_pose(left, XRT_INPUT_GENERIC_HEAD_POSE, 0, &relation) == XRT_ERROR_INPUT_UNSUPPORTED);
    pico_monado_producer_disconnect(producer);
    inactive(left);
    inactive(right);
    sample = event(1, 1, true);
    assert(!pico_monado_producer_push(producer, &sample));
    assert(!pico_monado_producer_begin(producer, 1, false, NULL, NULL));
    assert(pico_monado_producer_begin(producer, 2, false, &calibration, &calibration));
    assert(!pico_monado_producer_push(producer, &sample));
    sample = event(1, 2, true);
    assert(pico_monado_producer_push(producer, &sample));
    xrt_device_destroy(&right);
    inspect(left, true, true);
    pico_monado_producer_destroy(&producer);
    assert(!producer);
    inactive(left);
    xrt_device_destroy(&left);
    pico_monado_producer_destroy(&producer);
}

static void timeout_per_hand(void)
{
    struct pico_monado_producer *producer = NULL;
    struct xrt_device *left = NULL, *right = NULL;
    assert(pico_monado_devices_create(50000000, &producer, &left, &right));
    assert(pico_monado_producer_begin(producer, 1, false, &calibration, &calibration));
    struct pico_controller_event sample = event(1, 1, true);
    assert(pico_monado_producer_push(producer, &sample));
    os_nanosleep(80000000);
    sample = event(2, 1, true);
    assert(pico_monado_producer_push(producer, &sample));
    inactive(left);
    inspect(right, true, true);
    sample = event(1, 1, true);
    assert(pico_monado_producer_push(producer, &sample));
    inspect(left, true, true);
    xrt_device_destroy(&left);
    xrt_device_destroy(&right);
    sample = event(1, 1, false);
    assert(pico_monado_producer_push(producer, &sample));
    pico_monado_producer_destroy(&producer);
}

struct race {
    struct pico_monado_producer *producer;
    struct xrt_device *device;
    atomic_bool done;
    unsigned snapshots;
};

static void *reader(void *data)
{
    struct race *race = data;
    do {
        assert(xrt_device_update_inputs(race->device) == XRT_SUCCESS);
        bool pressed = false, active = false;
        float trigger = 0;
        int64_t timestamp = race->device->inputs[0].timestamp;
        for (unsigned i = 0; i < race->device->input_count; i++) {
            struct xrt_input *input = &race->device->inputs[i];
            assert(input->timestamp == timestamp);
            if (input->name == XRT_INPUT_PICO_NEO3_X_CLICK || input->name == XRT_INPUT_PICO_NEO3_A_CLICK) {
                pressed = input->value.boolean;
                active = input->active;
            }
            if (input->name == XRT_INPUT_PICO_NEO3_TRIGGER_VALUE) {
                trigger = input->value.vec1.x;
                assert(input->active == active);
            }
        }
        assert(trigger == (pressed ? 1.0f : 0.0f));
        race->snapshots++;
    } while (!atomic_load(&race->done));
    xrt_device_destroy(&race->device);
    return NULL;
}

static void concurrent_lifetime(void)
{
    struct pico_monado_producer *producer = NULL;
    struct xrt_device *left = NULL, *right = NULL;
    assert(pico_monado_devices_create(1000000000, &producer, &left, &right));
    struct race races[2] = {{.producer = producer, .device = left}, {.producer = producer, .device = right}};
    pthread_t threads[2];
    for (unsigned hand = 0; hand < 2; hand++)
        assert(!pthread_create(&threads[hand], NULL, reader, &races[hand]));
    for (uint64_t generation = 1; generation <= 1000; generation++) {
        assert(pico_monado_producer_begin(producer, generation, false, &calibration, &calibration));
        int64_t last[2] = {0};
        for (unsigned index = 0; index < 20; index++) {
            struct pico_controller_event sample = event(index % 2 + 1, generation, index % 4 < 2);
            while (sample.time.timestamp_ns <= last[index % 2])
                sample.time.timestamp_ns = (int64_t)os_monotonic_get_ns();
            last[index % 2] = sample.time.timestamp_ns;
            assert(pico_monado_producer_push(producer, &sample));
        }
        pico_monado_producer_disconnect(producer);
        if (generation == 500)
            atomic_store(&races[0].done, true);
    }
    pico_monado_producer_destroy(&producer);
    atomic_store(&races[1].done, true);
    for (unsigned hand = 0; hand < 2; hand++) {
        assert(!pthread_join(threads[hand], NULL));
        assert(races[hand].snapshots > 0);
    }
    printf("PICO_MONADO_DEVICE_CONCURRENCY_PASS events=20000 generations=1000 snapshots=%u\n",
           races[0].snapshots + races[1].snapshots);
}

int main(void)
{
    lifecycle();
    timeout_per_hand();
    concurrent_lifetime();
    puts("PICO_MONADO_DEVICE_PASS");
    return 0;
}
