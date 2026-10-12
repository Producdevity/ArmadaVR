#include "ssc-imu-session.h"

#include <gio/gio.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct callbacks {
    const char *mode;
    gpointer session;
    gint reports[2], invalidations, self_join_refused;
};

static bool calibrate(void *data, struct ssc_clock *clock)
{
    struct callbacks *callbacks = data;
    if (!strcmp(callbacks->mode, "clock-failure")) return false;
    int64_t anchor = g_get_monotonic_time() * 1000 - 100000000;
    return ssc_clock_init(clock, UINT64_C(0x123456789abcdef), 19200000, anchor, anchor, 0);
}

static void invalidate(void *data)
{
    struct callbacks *callbacks = data;
    g_assert_cmpint(g_atomic_int_add(&callbacks->invalidations, 1), ==, 0);
}

static void report(void *data, const struct ssc_imu_event *event)
{
    struct callbacks *callbacks = data;
    g_assert_cmpint(g_atomic_int_get(&callbacks->invalidations), ==, 0);
    g_assert_cmpuint(event->sensor, <, 2);
    g_assert_cmpfloat(event->vector.values[0], ==, event->sensor ? -0.125f : 9.75f);
    g_atomic_int_inc(&callbacks->reports[event->sensor]);
    struct ssc_imu_session *session = g_atomic_pointer_get(&callbacks->session);
    if (session) {
        struct ssc_imu_session_status status;
        ssc_imu_session_get_status(session, &status);
        g_assert_true(status.streaming);
        if (!strcmp(callbacks->mode, "callback-stop")) {
            struct ssc_imu_session *copy = session;
            g_assert_false(ssc_imu_session_destroy(&copy));
            g_assert_true(copy == session);
            g_atomic_int_set(&callbacks->self_join_refused, 1);
            ssc_imu_session_stop(session);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const char *modes[] = {"normal", "immediate", "no-service", "no-qrtr", "clock-failure",
                           "wrong-uid", "wrong-rate", "gyro-silent", "gyro-malformed", "gyro-stall",
                           "gyro-unavailable", "server-loss", "callback-stop"};
    bool known = false;
    for (unsigned i = 0; i < G_N_ELEMENTS(modes); i++) known |= !strcmp(argv[1], modes[i]);
    if (!known) return 2;
    bool offline = !strcmp(argv[1], "no-qrtr");
    if (offline) {
        int fd = socket(42, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) { close(fd); return 2; }
        if (errno != EAFNOSUPPORT && errno != EPROTONOSUPPORT) return 2;
    } else {
        gchar *identity = NULL, *command = NULL;
        bool virtual = g_file_get_contents("/proc/device-tree/compatible", &identity, NULL, NULL) &&
                       !strcmp(identity, "linux,dummy-virt") &&
                       g_file_get_contents("/proc/cmdline", &command, NULL, NULL) &&
                       strstr(command, "armada.ssc_test=1");
        g_free(identity); g_free(command);
        if (!virtual) return 2;
    }
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    unsigned cycles = offline ? 100 : !strcmp(argv[1], "normal") ? 3 : 1;
    for (unsigned cycle = 0; cycle < cycles; cycle++) {
        struct callbacks callbacks = {.mode = argv[1]};
        struct ssc_imu_session_config config = {
            .suid_high = {0xACC10001, 0xACC10002}, .suid_low = {0xACC20001, 0xACC20002},
            .sample_rate = {1000, 1000}, .max_age_ns = 2000000000,
            .startup_timeout_ns = 3000000000, .report_timeout_ns = 2000000000,
            .calibrate = calibrate, .report = report, .invalidate = invalidate, .data = &callbacks,
        };
        struct ssc_imu_session *session = NULL;
        for (unsigned invalid = 0; invalid < 9; invalid++) {
            struct ssc_imu_session_config bad = config;
            if (invalid == 0) bad.sample_rate[0] = NAN;
            if (invalid == 1) bad.sample_rate[1] = 0;
            if (invalid == 2) bad.suid_low[0] = bad.suid_high[0] = 0;
            if (invalid == 3) { bad.suid_low[1] = bad.suid_low[0]; bad.suid_high[1] = bad.suid_high[0]; }
            if (invalid == 4) bad.startup_timeout_ns = 0;
            if (invalid == 5) bad.max_age_ns = bad.report_timeout_ns + 1;
            if (invalid == 6) bad.report = NULL;
            if (invalid == 7) bad.invalidate = NULL;
            if (invalid == 8) bad.calibrate = NULL;
            g_assert_false(ssc_imu_session_create(&bad, &session)); g_assert_null(session);
        }
        if (!strcmp(argv[1], "wrong-uid")) config.suid_high[1]++;
        if (!strcmp(argv[1], "wrong-rate")) config.sample_rate[1] = 999;
        g_assert_true(ssc_imu_session_create(&config, &session));
        g_atomic_pointer_set(&callbacks.session, session);
        bool immediate = !strcmp(argv[1], "immediate") || (offline && cycle % 2 == 0);
        if (immediate) ssc_imu_session_stop(session);
        int64_t start = g_get_monotonic_time();
        bool stopped = immediate, announced = false;
        struct ssc_imu_session_status status = {0};
        while (!status.finished) {
            ssc_imu_session_get_status(session, &status);
            if (status.streaming && status.accepted[0] && status.accepted[1] && !announced) {
                puts("SSC_IMU_SESSION_STREAMING"); fflush(stdout); announced = true;
            }
            if (!strcmp(argv[1], "normal") && status.accepted[0] >= 4 && status.accepted[1] >= 4 && !stopped) {
                ssc_imu_session_stop(session); stopped = true;
            }
            g_assert_cmpint(g_get_monotonic_time() - start, <, 20000000);
            g_usleep(1000);
        }
        g_assert_false(status.streaming); g_assert_true(status.stopping);
        bool success = immediate || !strcmp(argv[1], "normal") || !strcmp(argv[1], "callback-stop");
        g_assert_cmpint(status.failed, ==, !success);
        if (!strcmp(argv[1], "normal")) {
            g_assert_cmpuint(status.accepted[0], >=, 4); g_assert_cmpuint(status.accepted[1], >=, 4);
            g_assert_cmpuint(status.rejected[0], >, 0); g_assert_cmpuint(status.rejected[1], >, 0);
        }
        if (!strcmp(argv[1], "wrong-uid") || !strcmp(argv[1], "wrong-rate"))
            g_assert_nonnull(strstr(status.error, "metadata"));
        if (!strcmp(argv[1], "clock-failure")) g_assert_nonnull(strstr(status.error, "clock"));
        if (!strcmp(argv[1], "gyro-silent")) {
            g_assert_nonnull(strstr(status.error, "Gyroscope report stream stopped"));
            g_assert_cmpuint(status.accepted[0], >, status.accepted[1]);
        }
        if (!strcmp(argv[1], "gyro-malformed")) {
            g_assert_nonnull(strstr(status.error, "Gyroscope report stream stopped"));
            g_assert_cmpuint(status.accepted[1], ==, 0);
        }
        if (!strcmp(argv[1], "gyro-stall")) {
            g_assert_nonnull(strstr(status.error, "startup deadline"));
            g_assert_cmpuint(status.accepted[0] + status.accepted[1], ==, 0);
        }
        if (!strcmp(argv[1], "callback-stop")) g_assert_cmpint(g_atomic_int_get(&callbacks.self_join_refused), ==, 1);
        g_assert_true(ssc_imu_session_destroy(&session)); g_assert_null(session);
        g_assert_cmpint(g_atomic_int_get(&callbacks.invalidations), ==, 1);
        for (unsigned i = 0; i < 2; i++)
            g_assert_cmpuint(status.accepted[i], ==, (unsigned)g_atomic_int_get(&callbacks.reports[i]));
        printf("SSC_IMU_SESSION_PASS mode=%s cycle=%u samples=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " failed=%d\n",
               argv[1], cycle, status.accepted[0], status.accepted[1], status.failed);
    }
    g_assert_true(ssc_imu_session_destroy(NULL));
    return 0;
}
