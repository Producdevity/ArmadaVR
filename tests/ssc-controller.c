#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libssc-sensor.h>
#include "pico-controller.h"
#include "ssc-clock.h"
#include "pico-controller-stream.h"
#include "pico-monado-device.h"
#include <os/os_time.h>

struct state {
    GMainLoop *loop;
    SSCSensor *sensor;
    GObject *client;
    guint64 high, low, last_ticks;
    struct ssc_clock clock;
    struct pico_controller_stream stream;
    uint64_t generation;
    struct pico_monado_producer *producer;
    struct xrt_device *devices[2];
    int64_t last_ns;
    unsigned samples, rejected, hands;
    gboolean opened, closing, closed, timed_out;
};

static void closed(GObject *object, GAsyncResult *result, gpointer data)
{
    struct state *state = data;
    GError *error = NULL;
    g_assert_true(ssc_sensor_close_finish(SSC_SENSOR(object), result, &error));
    g_assert_no_error(error);
    state->closed = TRUE;
    g_main_loop_quit(state->loop);
}

static void report(GObject *client, guint id, guint64 high, guint64 low,
                   guint64 timestamp, GBytes *payload, gpointer data)
{
    struct state *state = data;
    g_assert_true(client == state->client);
    if (id != 1024 || high != state->high || low != state->low || state->closing)
        return;
    gsize length;
    const void *bytes = g_bytes_get_data(payload, &length);
    struct pico_controller_event event;
    if (!pico_controller_stream_report(&state->stream, state->generation, id, high, low,
                                       timestamp, bytes, length, (int64_t)os_monotonic_get_ns(), &event)) {
        state->rejected++;
        return;
    }
    struct pico_controller_sample sample = event.sample;
    g_assert_true(pico_monado_producer_push(state->producer, &event));
    struct xrt_device *device = state->devices[sample.hand - 1];
    g_assert_cmpint(xrt_device_update_inputs(device), ==, XRT_SUCCESS);
    for (unsigned i = 0; i < PICO_MONADO_INPUT_COUNT; i++) {
        struct xrt_input *value = &device->inputs[i];
        g_assert_cmpint(value->timestamp, ==, event.time.timestamp_ns);
        if (value->name == XRT_INPUT_PICO_NEO3_TRIGGER_VALUE) {
            g_assert_true(value->active);
            g_assert_cmpfloat(value->value.vec1.x, ==, sample.trigger / 255.0f);
        }
        if (value->name == XRT_INPUT_PICO_NEO3_GRIP_POSE || value->name == XRT_INPUT_PICO_NEO3_AIM_POSE)
            g_assert_false(value->active);
    }
    g_assert_cmpint(event.time.timestamp_ns, >, state->last_ns);
    g_assert_cmpint(event.time.uncertainty_ns, ==, 2);
    state->last_ns = event.time.timestamp_ns;
    g_assert_cmpuint(event.accuracy, ==, 3);
    g_assert_cmpuint(timestamp, >, state->last_ticks);
    g_assert_cmpuint(timestamp, >, UINT64_C(0x123456789abcdef));
    state->last_ticks = timestamp;
    g_assert_cmpuint(sample.hand, >=, 1);
    g_assert_cmpuint(sample.hand, <=, 2);
    g_assert_cmpuint(sample.trigger, ==, sample.hand == 1 ? 0x89 : 0xb0);
    state->hands |= 1u << sample.hand;
    state->samples++;
    if (state->opened && state->samples >= 4 && state->hands == 6 && state->rejected) {
        state->closing = TRUE;
        pico_controller_stream_invalidate(&state->stream);
        pico_monado_producer_disconnect(state->producer);
        for (unsigned hand = 0; hand < 2; hand++) {
            g_assert_cmpint(xrt_device_update_inputs(state->devices[hand]), ==, XRT_SUCCESS);
            for (unsigned i = 0; i < PICO_MONADO_INPUT_COUNT; i++)
                g_assert_false(state->devices[hand]->inputs[i].active);
        }
        ssc_sensor_close(state->sensor, NULL, closed, state);
    }
}

static void opened(GObject *object, GAsyncResult *result, gpointer data)
{
    struct state *state = data;
    GError *error = NULL;
    g_assert_true(ssc_sensor_open_finish(SSC_SENSOR(object), result, &error));
    g_assert_no_error(error);
    state->opened = TRUE;
}

static void discovered(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)object;
    struct state *state = data;
    GError *error = NULL;
    state->sensor = ssc_sensor_new_finish(result, &error);
    g_assert_no_error(error);
    g_assert_nonnull(state->sensor);
    g_object_get(state->sensor, SSC_SENSOR_CLIENT, &state->client,
                 SSC_SENSOR_UID_HIGH, &state->high, SSC_SENSOR_UID_LOW, &state->low, NULL);
    g_assert_nonnull(state->client);
    g_assert_cmpuint(state->high, !=, 0);
    g_assert_cmpuint(state->low, !=, 0);
    struct pico_controller_stream_config config = {
        .clock = state->clock, .suid_high = state->high, .suid_low = state->low,
        .mcu_type = 1, .radio_type = 1, .max_age_ns = 10000000000,
    };
    g_assert_true(pico_controller_stream_begin(&state->stream, &config));
    state->generation = state->stream.generation;
    const struct pico_analog_calibration fixture_calibration = {
        .stick_x = {0, 128, 255, 0, false}, .stick_y = {0, 128, 255, 0, false},
        .trigger_pressed = 255, .grip_pressed = 255,
    };
    g_assert_true(pico_monado_producer_begin(state->producer, state->generation, false,
                                            &fixture_calibration, &fixture_calibration));
    g_signal_connect(state->client, "report-timestamped", G_CALLBACK(report), state);
    ssc_sensor_open(state->sensor, NULL, opened, state);
}

static gboolean deadline(gpointer data)
{
    struct state *state = data;
    state->timed_out = TRUE;
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "0") && strcmp(argv[2], "1")))
        return 2;
    gchar *identity = NULL, *command = NULL;
    if (!g_file_get_contents("/proc/device-tree/compatible", &identity, NULL, NULL) ||
        strcmp(identity, "linux,dummy-virt") ||
        !g_file_get_contents("/proc/cmdline", &command, NULL, NULL) ||
        !strstr(command, "armada.ssc_test=1")) {
        g_free(identity);
        g_free(command);
        return 2;
    }
    g_free(identity);
    g_free(command);
    gboolean expect_open = !strcmp(argv[2], "1");
    struct pico_controller_stream previous = {0};
    unsigned cycles = expect_open ? 3 : 1;
    for (unsigned cycle = 0; cycle < cycles; cycle++) {
        struct state state = {.loop = g_main_loop_new(NULL, FALSE), .stream = previous};
        int64_t anchor_ns = (int64_t)os_monotonic_get_ns() - 1000000000;
        g_assert_true(ssc_clock_init(&state.clock, UINT64_C(0x123456789abcdef),
                                     19200000, anchor_ns, anchor_ns, 0));
        g_assert_true(pico_monado_devices_create(10000000000, &state.producer,
                                                 &state.devices[0], &state.devices[1]));
        guint timer = g_timeout_add_seconds(4, deadline, &state);
        ssc_sensor_new(argv[1], NULL, discovered, &state);
        g_main_loop_run(state.loop);
        if (!state.timed_out)
            g_source_remove(timer);
        g_assert_cmpuint(state.samples, >=, 4);
        g_assert_cmpuint(state.hands, ==, 6);
        g_assert_cmpuint(state.rejected, >, 0);
        g_assert_true(state.opened == expect_open);
        g_assert_true(state.closed == expect_open);
        g_assert_true(state.timed_out != expect_open);
        g_assert_cmpuint(state.generation, ==, cycle + 1);
        printf("SSC_CONTROLLER_PASS type=%s opened=%d closed=%d samples=%u rejected=%u hands=%u\n",
               argv[1], state.opened, state.closed, state.samples, state.rejected, state.hands);
        if (state.client)
            g_signal_handlers_disconnect_by_data(state.client, &state);
        pico_controller_stream_invalidate(&state.stream);
        previous = state.stream;
        g_clear_object(&state.sensor);
        g_clear_object(&state.client);
        pico_monado_producer_destroy(&state.producer);
        for (unsigned hand = 0; hand < 2; hand++) {
            g_assert_cmpint(xrt_device_update_inputs(state.devices[hand]), ==, XRT_SUCCESS);
            for (unsigned i = 0; i < PICO_MONADO_INPUT_COUNT; i++)
                g_assert_false(state.devices[hand]->inputs[i].active);
            xrt_device_destroy(&state.devices[hand]);
        }
        g_main_loop_unref(state.loop);
    }
    if (expect_open)
        printf("SSC_CONTROLLER_RECONNECT_PASS cycles=%u\n", cycles);
    printf("SSC_MONADO_INPUT_PASS cycles=%u\n", cycles);
    printf("SSC_MONADO_DEVICE_PASS cycles=%u\n", cycles);
    return 0;
}
