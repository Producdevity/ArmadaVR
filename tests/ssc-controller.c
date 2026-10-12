#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libssc-sensor.h>
#include "pico-controller.h"
#include "ssc-clock.h"

struct state {
    GMainLoop *loop;
    SSCSensor *sensor;
    GObject *client;
    guint64 high, low, last_ticks;
    struct ssc_clock clock;
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
    struct pico_controller_sample sample;
    unsigned accuracy;
    if (!pico_controller_decode_ssc(bytes, length, 1, 1, FALSE, &sample, &accuracy)) {
        state->rejected++;
        return;
    }
    struct ssc_timestamp aligned;
    g_assert_true(ssc_clock_map(&state->clock, timestamp, 2000000000, 2000000000, &aligned));
    g_assert_cmpint(aligned.timestamp_ns, >, state->last_ns);
    g_assert_cmpint(aligned.uncertainty_ns, ==, 2);
    state->last_ns = aligned.timestamp_ns;
    g_assert_cmpuint(accuracy, ==, 3);
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
    struct state state = {.loop = g_main_loop_new(NULL, FALSE)};
    g_assert_true(ssc_clock_init(&state.clock, UINT64_C(0x123456789abcdef),
                                 19200000, 1000000000, 1000000000, 0));
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
    printf("SSC_CONTROLLER_PASS type=%s opened=%d closed=%d samples=%u rejected=%u hands=%u\n",
           argv[1], state.opened, state.closed, state.samples, state.rejected, state.hands);
    if (state.client)
        g_signal_handlers_disconnect_by_data(state.client, &state);
    g_clear_object(&state.sensor);
    g_clear_object(&state.client);
    g_main_loop_unref(state.loop);
    return 0;
}
