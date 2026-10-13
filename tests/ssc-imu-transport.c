#include <libssc-sensor.h>
#include "ssc-imu.h"
#include "ssc-calibration.h"
#include <stdio.h>
#include <string.h>

struct state {
    GMainLoop *loop;
    SSCSensor *sensors[2];
    GObject *clients[2];
    struct ssc_imu_stream stream;
    struct ssc_imu_config config;
    struct ssc_calibration calibration;
    unsigned discovered, opened, closed, samples[2], rejected[2];
    gboolean closing, timed_out, calibration_mode;
};

static void closed(GObject *object, GAsyncResult *result, gpointer data)
{
    struct state *s = data;
    GError *error = NULL;
    g_assert_true(ssc_sensor_close_finish(SSC_SENSOR(object), result, &error));
    g_assert_no_error(error);
    if (++s->closed == 2) g_main_loop_quit(s->loop);
}

static void received(GObject *client, guint id, guint64 high, guint64 low,
                     guint64 ticks, GBytes *payload, gpointer data)
{
    struct state *s = data;
    unsigned sensor = client == s->clients[0] ? 0 : 1;
    g_assert_true(client == s->clients[sensor]);
    if (s->closing) return;
    gsize length;
    const void *bytes = g_bytes_get_data(payload, &length);
    struct ssc_imu_event event;
    int64_t now = g_get_monotonic_time() * 1000;
    bool accepted;
    if (s->calibration_mode) {
        accepted = ssc_calibration_report(&s->calibration, s->stream.generation, id, high, low,
                                         ticks, bytes, length, now);
        if (accepted) {
            struct ssc_imu_event raw = {.sensor = sensor, .generation = s->stream.generation,
                .ticks = ticks, .vector = {.values = {4, 5, 6}, .accuracy = 3}};
            g_assert_true(ssc_clock_map(&s->config.clock, ticks, now, s->config.max_age_ns, &raw.time));
            struct ssc_bias_sample corrected;
            g_assert_true(ssc_calibration_correct(&s->calibration, &raw, now, &corrected));
            g_assert_cmpfloat(corrected.corrected[0], ==, sensor ? 4.125f : -5.75f);
            g_assert_cmpfloat(corrected.corrected[1], ==, 3.0f);
            g_assert_cmpfloat(corrected.corrected[2], ==, 9.5f);
            g_assert_cmpuint(corrected.accuracy, ==, 3);
            event = raw;
            memcpy(event.vector.values, corrected.calibration.value.bias, sizeof(event.vector.values));
        }
    } else {
        accepted = ssc_imu_report(&s->stream, s->stream.generation, id, high, low, ticks,
                                  bytes, length, now, &event);
    }
    if (!accepted) {
        s->rejected[sensor]++;
        return;
    }
    g_assert_cmpuint(event.sensor, ==, sensor);
    g_assert_cmpuint(event.vector.accuracy, ==, 3);
    g_assert_cmpfloat(event.vector.values[0], ==, sensor ? -0.125f : 9.75f);
    g_assert_cmpfloat(event.vector.values[1], ==, 2.0f);
    g_assert_cmpfloat(event.vector.values[2], ==, -3.5f);
    g_assert_cmpint(event.time.uncertainty_ns, ==, 2);
    s->samples[sensor]++;
    if (s->opened == 2 && s->samples[0] >= 4 && s->samples[1] >= 4 && s->rejected[0] && s->rejected[1]) {
        s->closing = TRUE;
        ssc_imu_invalidate(&s->stream);
        ssc_calibration_invalidate(&s->calibration);
        g_assert_false(ssc_imu_report(&s->stream, s->stream.generation, id, high, low, ticks,
                                      bytes, length, g_get_monotonic_time() * 1000, &event));
        for (unsigned i = 0; i < 2; i++) ssc_sensor_close(s->sensors[i], NULL, closed, s);
    }
}

static void opened(GObject *object, GAsyncResult *result, gpointer data)
{
    struct state *s = data;
    GError *error = NULL;
    g_assert_true(ssc_sensor_open_finish(SSC_SENSOR(object), result, &error));
    g_assert_no_error(error);
    s->opened++;
}

static void discovered(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)object;
    struct state *s = data;
    unsigned sensor = s->discovered;
    GError *error = NULL;
    s->sensors[sensor] = ssc_sensor_new_finish(result, &error);
    g_assert_no_error(error);
    g_assert_nonnull(s->sensors[sensor]);
    const char *names[] = {SSC_SENSOR_STREAM_TYPE, SSC_SENSOR_AVAILABLE, SSC_SENSOR_SAMPLE_RATE};
    const GType types[] = {G_TYPE_UINT, G_TYPE_BOOLEAN, G_TYPE_FLOAT};
    for (unsigned i = 0; i < G_N_ELEMENTS(names); i++) {
        GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(s->sensors[sensor]), names[i]);
        g_assert_nonnull(spec);
        g_assert_cmpuint(G_PARAM_SPEC_VALUE_TYPE(spec), ==, types[i]);
    }
    guint stream_type = 99;
    gboolean available = FALSE;
    gfloat sample_rate = 0;
    g_object_get(s->sensors[sensor], SSC_SENSOR_STREAM_TYPE, &stream_type,
                 SSC_SENSOR_AVAILABLE, &available, SSC_SENSOR_SAMPLE_RATE, &sample_rate, NULL);
    g_assert_cmpuint(stream_type, ==, s->calibration_mode ? 1 : 0);
    g_assert_true(available);
    g_assert_cmpfloat(sample_rate, ==, s->calibration_mode ? 0.0f : 1000.0f);
    GParamSpec *body_spec = g_object_class_find_property(G_OBJECT_GET_CLASS(s->sensors[sensor]), "rigid-body");
    g_assert_nonnull(body_spec);
    g_assert_cmpuint(G_PARAM_SPEC_VALUE_TYPE(body_spec), ==, G_TYPE_INT);
    gint body = -1;
    g_object_get(s->sensors[sensor], "rigid-body", &body, NULL);
    g_assert_cmpint(body, ==, 0);
    g_object_get(s->sensors[sensor], SSC_SENSOR_CLIENT, &s->clients[sensor],
                 SSC_SENSOR_UID_HIGH, &s->config.suid_high[sensor],
                 SSC_SENSOR_UID_LOW, &s->config.suid_low[sensor], NULL);
    if (++s->discovered == 1) {
        ssc_sensor_new(s->calibration_mode ? "gyro_cal" : "gyro", NULL, discovered, s);
        return;
    }
    uint64_t old = s->stream.generation;
    g_assert_true(ssc_imu_begin(&s->stream, &s->config));
    if (s->calibration_mode) {
        struct ssc_calibration_config config = {.clock = s->config.clock,
            .max_report_age_ns = s->config.max_age_ns, .max_bias_age_ns = 1000000000};
        memcpy(config.suid_high, s->config.suid_high, sizeof(config.suid_high));
        memcpy(config.suid_low, s->config.suid_low, sizeof(config.suid_low));
        g_assert_true(ssc_calibration_begin(&s->calibration, &config, s->stream.generation));
    }
    struct ssc_imu_event event;
    g_assert_false(ssc_imu_report(&s->stream, old, 1025, s->config.suid_high[0], s->config.suid_low[0],
                                  0, NULL, 0, g_get_monotonic_time() * 1000, &event));
    for (unsigned i = 0; i < 2; i++) {
        g_signal_connect(s->clients[i], "report-timestamped", G_CALLBACK(received), s);
        ssc_sensor_open(s->sensors[i], NULL, opened, s);
    }
}

static gboolean deadline(gpointer data)
{
    struct state *s = data;
    s->timed_out = TRUE;
    g_main_loop_quit(s->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "calibration"))) return 2;
    gchar *identity = NULL, *command = NULL;
    if (!g_file_get_contents("/proc/device-tree/compatible", &identity, NULL, NULL) ||
        strcmp(identity, "linux,dummy-virt") || !g_file_get_contents("/proc/cmdline", &command, NULL, NULL) ||
        !strstr(command, "armada.ssc_test=1")) {
        g_free(identity); g_free(command);
        return 2;
    }
    g_free(identity); g_free(command);
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    struct ssc_imu_stream previous = {0};
    for (unsigned cycle = 0; cycle < 3; cycle++) {
        GMainContext *context = g_main_context_new();
        g_main_context_push_thread_default(context);
        struct state s = {.loop = g_main_loop_new(context, FALSE), .stream = previous,
                         .calibration_mode = argc == 2};
        int64_t anchor = g_get_monotonic_time() * 1000 - 1000000000;
        g_assert_true(ssc_clock_init(&s.config.clock, UINT64_C(0x123456789abcdef), 19200000, anchor, anchor, 0));
        s.config.max_age_ns = 10000000000;
        GSource *timer = g_timeout_source_new_seconds(8);
        g_source_set_callback(timer, deadline, &s, NULL);
        g_source_attach(timer, context);
        ssc_sensor_new(s.calibration_mode ? "accel_cal" : "accel", NULL, discovered, &s);
        g_main_loop_run(s.loop);
        g_source_destroy(timer); g_source_unref(timer);
        g_assert_false(s.timed_out);
        g_assert_cmpuint(s.opened, ==, 2); g_assert_cmpuint(s.closed, ==, 2);
        g_assert_cmpuint(s.stream.generation, ==, cycle + 1);
        printf("SSC_%s_TRANSPORT_PASS cycle=%u accel=%u gyro=%u rejected=%u/%u\n",
               s.calibration_mode ? "CALIBRATION" : "IMU", cycle,
               s.samples[0], s.samples[1], s.rejected[0], s.rejected[1]);
        for (unsigned i = 0; i < 2; i++) {
            g_signal_handlers_disconnect_by_data(s.clients[i], &s);
            g_clear_object(&s.sensors[i]); g_clear_object(&s.clients[i]);
        }
        previous = s.stream;
        g_main_loop_unref(s.loop);
        g_main_context_pop_thread_default(context); g_main_context_unref(context);
    }
    return 0;
}
