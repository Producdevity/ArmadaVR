#include "pico-ssc-session.h"

#include <gio/gio.h>
#include <os/os_time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool calibrate(void *data, struct ssc_clock *clock)
{
    if (!strcmp(data, "clock-failure"))
        return false;
    int64_t anchor = (int64_t)os_monotonic_get_ns() - 100000000;
    return ssc_clock_init(clock, UINT64_C(0x123456789abcdef), 19200000, anchor, anchor, 0);
}

static void inactive(struct xrt_device *device)
{
    g_assert_cmpint(xrt_device_update_inputs(device), ==, XRT_SUCCESS);
    union xrt_input_value zero = {0};
    for (unsigned i = 0; i < device->input_count; i++) {
        g_assert_false(device->inputs[i].active);
        g_assert_cmpmem(&device->inputs[i].value, sizeof(zero), &zero, sizeof(zero));
    }
}

static unsigned inspect(struct xrt_device *device)
{
    g_assert_cmpint(xrt_device_update_inputs(device), ==, XRT_SUCCESS);
    unsigned active = 0;
    for (unsigned i = 0; i < device->input_count; i++) {
        if (device->inputs[i].name == XRT_INPUT_PICO_NEO3_TRIGGER_VALUE && device->inputs[i].active) {
            float expected = device->device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER ? 137.0f : 176.0f;
            g_assert_cmpfloat(device->inputs[i].value.vec1.x, ==, expected / 255.0f);
            active++;
        }
        if (device->inputs[i].name == XRT_INPUT_PICO_NEO3_AIM_POSE ||
            device->inputs[i].name == XRT_INPUT_PICO_NEO3_GRIP_POSE)
            g_assert_false(device->inputs[i].active);
    }
    return active;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const char *modes[] = {"normal", "immediate", "no-service", "clock-failure", "late",
                           "silent", "malformed", "server-loss", "no-qrtr"};
    bool known = false;
    for (unsigned i = 0; i < G_N_ELEMENTS(modes); i++)
        known |= !strcmp(argv[1], modes[i]);
    if (!known)
        return 2;
    bool offline = !strcmp(argv[1], "no-qrtr");
    if (offline) {
        int fd = socket(42, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            close(fd);
            return 2;
        }
        if (errno != EAFNOSUPPORT && errno != EPROTONOSUPPORT)
            return 2;
    } else {
        gchar *identity = NULL, *command = NULL;
        bool virtual = g_file_get_contents("/proc/device-tree/compatible", &identity, NULL, NULL) &&
                       !strcmp(identity, "linux,dummy-virt") &&
                       g_file_get_contents("/proc/cmdline", &command, NULL, NULL) &&
                       strstr(command, "armada.ssc_test=1");
        g_free(identity);
        g_free(command);
        if (!virtual)
            return 2;
    }
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    const struct pico_analog_calibration calibration = {
        .stick_x = {0, 128, 255, 0, false}, .stick_y = {0, 128, 255, 0, false},
        .trigger_pressed = 255, .grip_pressed = 255,
    };
    struct pico_ssc_session_config config = {
        .calibrate = calibrate, .clock_data = argv[1], .max_age_ns = 2000000000,
        .timeout_ns = 2000000000, .mcu_type = 1, .radio_type = 1,
        .left = &calibration, .right = &calibration,
    };
    unsigned cycles = offline ? 100 : !strcmp(argv[1], "normal") ? 3 : 1;
    for (unsigned cycle = 0; cycle < cycles; cycle++) {
        struct pico_ssc_session *session = NULL;
        struct xrt_device *devices[2] = {NULL, NULL};
        struct pico_ssc_session_config invalid = config;
        invalid.mcu_type = 0;
        g_assert_false(pico_ssc_session_create(&invalid, &session, &devices[0], &devices[1]));
        g_assert_null(session);
        g_assert_null(devices[0]);
        g_assert_null(devices[1]);
        g_assert_true(pico_ssc_session_create(&config, &session, &devices[0], &devices[1]));
        bool immediate = !strcmp(argv[1], "immediate") || (offline && cycle % 2 == 0);
        if (immediate)
            pico_ssc_session_stop(session);
        int64_t start = (int64_t)os_monotonic_get_ns();
        unsigned hands = 0;
        bool stopped = immediate, announced = false;
        struct pico_ssc_session_status status = {0};
        while (!status.finished) {
            pico_ssc_session_get_status(session, &status);
            for (unsigned hand = 0; hand < 2; hand++)
                if (devices[hand] && inspect(devices[hand]))
                    hands |= 1u << hand;
            if (!announced && status.streaming && hands == 3) {
                puts("SSC_SESSION_STREAMING");
                fflush(stdout);
                announced = true;
            }
            if (!stopped && status.streaming && hands == 3 && status.accepted >= 4 &&
                (!strcmp(argv[1], "normal") || !strcmp(argv[1], "late"))) {
                xrt_device_destroy(&devices[0]);
                pico_ssc_session_stop(session);
                stopped = true;
            }
            g_assert_cmpint((int64_t)os_monotonic_get_ns() - start, <, 20000000000);
            g_usleep(1000);
        }
        g_assert_false(status.streaming);
        g_assert_true(status.stopping);
        if (offline || !strcmp(argv[1], "no-service") || !strcmp(argv[1], "clock-failure"))
            g_assert_cmpuint(status.accepted, ==, 0);
        if ((offline && !immediate) || !strcmp(argv[1], "no-service") || !strcmp(argv[1], "clock-failure"))
            g_assert_true(status.failed);
        if (!strcmp(argv[1], "clock-failure"))
            g_assert_nonnull(strstr(status.error, "clock"));
        if (!strcmp(argv[1], "silent") || !strcmp(argv[1], "malformed") || !strcmp(argv[1], "server-loss")) {
            g_assert_true(status.failed);
            g_assert_nonnull(strstr(status.error, "stream stopped"));
            if (strcmp(argv[1], "malformed"))
                g_assert_cmpuint(hands, ==, 3);
        }
        if (!strcmp(argv[1], "normal") || !strcmp(argv[1], "late")) {
            g_assert_false(status.failed);
            g_assert_cmpuint(hands, ==, 3);
            g_assert_cmpuint(status.accepted, >=, 4);
            g_assert_cmpuint(status.rejected, >, 0);
        }
        pico_ssc_session_destroy(&session);
        g_assert_null(session);
        for (unsigned hand = 0; hand < 2; hand++) {
            if (devices[hand])
                inactive(devices[hand]);
            xrt_device_destroy(&devices[hand]);
        }
        printf("SSC_SESSION_PASS mode=%s cycle=%u accepted=%" G_GUINT64_FORMAT " rejected=%" G_GUINT64_FORMAT
               " failed=%d\n", argv[1], cycle, status.accepted, status.rejected, status.failed);
    }
    return 0;
}
