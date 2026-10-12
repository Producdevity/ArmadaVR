#include "pico-monado-device.h"

#include <os/os_time.h>
#include <util/u_device.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pico_monado_producer {
    pthread_mutex_t lock;
    struct pico_monado_input hands[2];
    unsigned references;
    uint64_t generation;
    int64_t timeout_ns;
    bool connected;
};

struct pico_monado_device {
    struct xrt_device base;
    struct xrt_tracking_origin origin;
    struct xrt_input inputs[PICO_MONADO_INPUT_COUNT];
    struct pico_monado_producer *producer;
    unsigned hand;
};

static void release(struct pico_monado_producer *producer)
{
    pthread_mutex_lock(&producer->lock);
    bool last = --producer->references == 0;
    pthread_mutex_unlock(&producer->lock);
    if (last) {
        pthread_mutex_destroy(&producer->lock);
        free(producer);
    }
}

static xrt_result_t update_inputs(struct xrt_device *xdev)
{
    struct pico_monado_device *device = (struct pico_monado_device *)xdev;
    struct pico_monado_producer *producer = device->producer;
    pthread_mutex_lock(&producer->lock);
    const struct pico_monado_input *input = &producer->hands[device->hand];
    int64_t now = (int64_t)os_monotonic_get_ns();
    memcpy(device->inputs, input->inputs, sizeof(device->inputs));
    if (!producer->connected || !input->seen || now < input->last_time_ns ||
        now - input->last_time_ns > producer->timeout_ns) {
        for (unsigned i = 0; i < PICO_MONADO_INPUT_COUNT; i++) {
            device->inputs[i].active = false;
            device->inputs[i].timestamp = now;
            memset(&device->inputs[i].value, 0, sizeof(device->inputs[i].value));
        }
    }
    pthread_mutex_unlock(&producer->lock);
    return XRT_SUCCESS;
}

static xrt_result_t get_tracked_pose(struct xrt_device *xdev, enum xrt_input_name name,
                                    int64_t at_timestamp_ns, struct xrt_space_relation *relation)
{
    (void)xdev;
    (void)at_timestamp_ns;
    if (name != XRT_INPUT_PICO_NEO3_GRIP_POSE && name != XRT_INPUT_PICO_NEO3_AIM_POSE)
        return XRT_ERROR_INPUT_UNSUPPORTED;
    memset(relation, 0, sizeof(*relation));
    relation->pose.orientation.w = 1;
    return XRT_SUCCESS;
}

static void destroy(struct xrt_device *xdev)
{
    struct pico_monado_device *device = (struct pico_monado_device *)xdev;
    release(device->producer);
    free(device);
}

bool pico_monado_devices_create(int64_t timeout_ns,
                                struct pico_monado_producer **producer_out,
                                struct xrt_device **left_out, struct xrt_device **right_out)
{
    if (timeout_ns <= 0 || !producer_out || !left_out || !right_out || left_out == right_out ||
        *producer_out || *left_out || *right_out)
        return false;
    struct pico_monado_producer *producer = calloc(1, sizeof(*producer));
    struct pico_monado_device *devices[2] = {calloc(1, sizeof(*devices[0])), calloc(1, sizeof(*devices[0]))};
    if (!producer || !devices[0] || !devices[1] || pthread_mutex_init(&producer->lock, NULL)) {
        free(producer);
        free(devices[0]);
        free(devices[1]);
        return false;
    }
    producer->references = 3;
    producer->timeout_ns = timeout_ns;
    for (unsigned hand = 0; hand < 2; hand++) {
        struct pico_monado_device *device = devices[hand];
        struct xrt_device *xdev = &device->base;
        device->producer = producer;
        device->hand = hand;
        pico_monado_input_begin(&producer->hands[hand], hand + 1, 1, false, NULL);
        memcpy(device->inputs, producer->hands[hand].inputs, sizeof(device->inputs));
        u_device_populate_function_pointers(xdev, get_tracked_pose, destroy);
        xdev->update_inputs = update_inputs;
        xdev->name = XRT_DEVICE_PICO_NEO3_CONTROLLER;
        xdev->device_type = hand ? XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER : XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER;
        snprintf(xdev->str, sizeof(xdev->str), "Pico Neo3 %s controller", hand ? "right" : "left");
        snprintf(xdev->serial, sizeof(xdev->serial), "Pico Neo3 %s", hand ? "right" : "left");
        xdev->tracking_origin = &device->origin;
        device->origin.type = XRT_TRACKING_TYPE_NONE;
        device->origin.initial_offset.orientation.w = 1;
        snprintf(device->origin.name, sizeof(device->origin.name), "Pico Neo3 input");
        xdev->input_count = PICO_MONADO_INPUT_COUNT;
        xdev->inputs = device->inputs;
    }
    *producer_out = producer;
    *left_out = &devices[0]->base;
    *right_out = &devices[1]->base;
    return true;
}

bool pico_monado_producer_begin(struct pico_monado_producer *producer,
                                uint64_t generation, bool touch_supported,
                                const struct pico_analog_calibration *left,
                                const struct pico_analog_calibration *right)
{
    struct pico_monado_input hands[2];
    if (!producer || !pico_monado_input_begin(&hands[0], 1, generation, touch_supported, left) ||
        !pico_monado_input_begin(&hands[1], 2, generation, touch_supported, right))
        return false;
    pthread_mutex_lock(&producer->lock);
    bool accepted = generation > producer->generation;
    if (accepted) {
        memcpy(producer->hands, hands, sizeof(hands));
        producer->generation = generation;
        producer->connected = true;
    }
    pthread_mutex_unlock(&producer->lock);
    return accepted;
}

bool pico_monado_producer_push(struct pico_monado_producer *producer,
                               const struct pico_controller_event *event)
{
    if (!producer || !event || event->sample.hand < 1 || event->sample.hand > 2 || event->time.timestamp_ns < 0)
        return false;
    pthread_mutex_lock(&producer->lock);
    int64_t now = (int64_t)os_monotonic_get_ns();
    bool accepted = producer->connected && event->time.timestamp_ns <= now &&
                    now - event->time.timestamp_ns <= producer->timeout_ns &&
                    pico_monado_input_update(&producer->hands[event->sample.hand - 1], event);
    pthread_mutex_unlock(&producer->lock);
    return accepted;
}

void pico_monado_producer_disconnect(struct pico_monado_producer *producer)
{
    if (!producer)
        return;
    pthread_mutex_lock(&producer->lock);
    producer->connected = false;
    pthread_mutex_unlock(&producer->lock);
}

void pico_monado_producer_destroy(struct pico_monado_producer **producer)
{
    if (!producer || !*producer)
        return;
    struct pico_monado_producer *old = *producer;
    *producer = NULL;
    pico_monado_producer_disconnect(old);
    release(old);
}
