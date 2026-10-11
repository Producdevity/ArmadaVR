#include <android/looper.h>
#include <android/sensor.h>
#include <android/hardware_buffer.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static volatile sig_atomic_t stopped;

static void stop(int signal_number)
{
    (void)signal_number;
    stopped = 1;
}

static int64_t now(clockid_t clock)
{
    struct timespec time;
    if (clock_gettime(clock, &time))
        return -1;
    return (int64_t)time.tv_sec * INT64_C(1000000000) + time.tv_nsec;
}

static bool number(const char *text, long minimum, long maximum, long *value)
{
    char *end;
    errno = 0;
    long parsed = strtol(text, &end, 0);
    if (errno || end == text || *end || parsed < minimum || parsed > maximum)
        return false;
    *value = parsed;
    return true;
}

int main(int argc, char **argv)
{
    long handle = -1, second_handle = -1, seconds = 5, period_us = 5000;
    const char *direct_path = NULL;
    if (argc >= 3 && !strcmp(argv[1], "--direct")) {
        direct_path = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc != 1 && ((argc != 4 && argc != 5) || !number(argv[1], 0, INT32_MAX, &handle) ||
                     !number(argv[2], 1, 30, &seconds) ||
                     !number(argv[3], 1250, 1000000, &period_us) ||
                     (argc == 5 && (!number(argv[4], 0, INT32_MAX, &second_handle) ||
                                    second_handle == handle)))) {
        fprintf(stderr, "usage: android-sensor-probe [--direct new-file] [handle seconds(1..30) period_us(1250..1000000) [second_handle]]\n");
        return 2;
    }
    if (direct_path && handle < 0)
        return 2;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    signal(SIGPIPE, SIG_IGN);
    umask(077);
    ASensorManager *manager = ASensorManager_getInstanceForPackage("com.android.shell");
    if (!manager)
        return 3;
    ASensorList sensors;
    int count = ASensorManager_getSensorList(manager, &sensors);
    const ASensor *selected[2] = {NULL, NULL};
    unsigned selected_count = second_handle < 0 ? 1 : 2;
    for (int i = 0; i < count; ++i) {
        const ASensor *sensor = sensors[i];
        /* Quest sensor names contain private unit calibration. */
        printf("{\"sensor\":%d,\"type\":%d,\"minimum_period_us\":%d,"
               "\"reporting_mode\":%d,\"direct_rate\":%d}\n",
               ASensor_getHandle(sensor), ASensor_getType(sensor),
               ASensor_getMinDelay(sensor), ASensor_getReportingMode(sensor),
               ASensor_getHighestDirectReportRateLevel(sensor));
        if (ASensor_getHandle(sensor) == handle)
            selected[0] = sensor;
        if (ASensor_getHandle(sensor) == second_handle)
            selected[1] = sensor;
    }
    if (handle < 0)
        return count < 0 ? 3 : 0;
    for (unsigned i = 0; i < selected_count; ++i) {
        if (!selected[i] || ASensor_getReportingMode(selected[i]) != AREPORTING_MODE_CONTINUOUS) {
            fprintf(stderr, "selected sensor is missing or not continuous\n");
            return 4;
        }
    }
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorEventQueue *queue = ASensorManager_createEventQueue(manager, looper, 1, NULL, NULL);
    if (!queue)
        return 5;
    int error = 0;
    AHardwareBuffer *buffer = NULL;
    FILE *direct_output = NULL;
    int channel = -1;
    const size_t direct_size = 104 * 32768;
    if (direct_path) {
        direct_output = fopen(direct_path, "wbx");
        if (!direct_output) {
            ASensorManager_destroyEventQueue(manager, queue);
            return 5;
        }
        if (!ASensor_isDirectChannelTypeSupported(selected[0], ASENSOR_DIRECT_CHANNEL_TYPE_HARDWARE_BUFFER)) {
            error = -ENOTSUP;
        } else {
            AHardwareBuffer_Desc descriptor = {
                .width = direct_size, .height = 1, .layers = 1,
                .format = AHARDWAREBUFFER_FORMAT_BLOB,
                .usage = AHARDWAREBUFFER_USAGE_SENSOR_DIRECT_DATA | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY,
            };
            error = AHardwareBuffer_allocate(&descriptor, &buffer);
            if (!error) {
                channel = ASensorManager_createHardwareBufferDirectChannel(manager, buffer, direct_size);
                if (channel <= 0)
                    error = channel ? channel : -EIO;
            }
            if (!error) {
                int token = ASensorManager_configureDirectReport(manager, selected[0], channel, ASENSOR_DIRECT_RATE_VERY_FAST);
                fprintf(stderr, "direct_channel=%d token=%d\n", channel, token);
                /* Quest's vendor service returns zero while activating packet-80 delivery. */
                if (token < 0)
                    error = token;
            }
        }
    }
    unsigned attempted = 0;
    for (; attempted < selected_count && !error && !direct_path; ++attempted) {
        error = ASensorEventQueue_registerSensor(queue, selected[attempted], (int32_t)period_us, 0);
        fprintf(stderr, "register=%d handle=%d period_us=%ld\n", error,
                ASensor_getHandle(selected[attempted]), period_us);
    }
    size_t received = 0;
    int64_t start = now(CLOCK_MONOTONIC);
    if (!error && start < 0)
        error = -EIO;
    while (!error && !stopped && received < 30000) {
        int64_t current = now(CLOCK_MONOTONIC);
        if (current < 0) {
            error = -EIO;
            break;
        }
        if (current - start >= seconds * INT64_C(1000000000))
            break;
        int polled = ALooper_pollOnce(100, NULL, NULL, NULL);
        if (polled == ALOOPER_POLL_ERROR) {
            error = -EIO;
            break;
        }
        ASensorEvent events[32];
        ssize_t length = ASensorEventQueue_getEvents(queue, events, 32);
        if (length < 0) {
            error = (int)length;
            break;
        }
        int64_t host_time = now(CLOCK_BOOTTIME);
        int64_t monotonic = now(CLOCK_MONOTONIC);
        for (ssize_t i = 0; i < length; ++i) {
            const ASensorEvent *event = &events[i];
            printf("{\"event_sensor\":%d,\"type\":%d,\"version\":%d,"
                   "\"timestamp_raw\":%" PRId64 ",\"received_boottime_ns\":%" PRId64
                   ",\"received_monotonic_ns\":%" PRId64
                   ",\"payload_hex\":\"", event->sensor, event->type, event->version,
                   event->timestamp, host_time, monotonic);
            const unsigned char *bytes = (const unsigned char *)event->data;
            for (size_t j = 0; j < sizeof(event->data); ++j)
                printf("%02x", bytes[j]);
            puts("\"}");
            ++received;
        }
        if (ferror(stdout))
            error = -EIO;
    }
    int disabled = 0;
    for (unsigned i = 0; i < attempted; ++i) {
        int result = ASensorEventQueue_disableSensor(queue, selected[i]);
        if (result && !disabled)
            disabled = result;
    }
    int destroyed = ASensorManager_destroyEventQueue(manager, queue);
    size_t direct_received = 0;
    if (channel > 0) {
        int result = ASensorManager_configureDirectReport(manager, NULL, channel, ASENSOR_DIRECT_RATE_STOP);
        fprintf(stderr, "direct_stop=%d\n", result);
        if (result < 0 && !error)
            error = result;
        ASensorManager_destroyDirectChannel(manager, channel);
    }
    if (buffer) {
        void *data = NULL;
        int result = AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &data);
        if (!result) {
            if (fwrite(data, 1, direct_size, direct_output) != direct_size)
                error = -EIO;
            for (size_t i = 0; i < direct_size / 104; ++i) {
                const unsigned char *slot = (const unsigned char *)data + i * 104;
                uint32_t counter = (uint32_t)slot[12] | (uint32_t)slot[13] << 8 |
                                   (uint32_t)slot[14] << 16 | (uint32_t)slot[15] << 24;
                if (!counter)
                    continue;
                ++direct_received;
                if (counter != i + 1 || counter != direct_received)
                    error = -EOVERFLOW;
            }
            int unlocked = AHardwareBuffer_unlock(buffer, NULL);
            if (unlocked && !error)
                error = unlocked;
        } else if (!error) {
            error = result;
        }
        AHardwareBuffer_release(buffer);
    }
    if (direct_output && fclose(direct_output) && !error)
        error = -EIO;
    if (fflush(stdout) && !error)
        error = -EIO;
    fprintf(stderr, "events=%zu direct_events=%zu error=%d disable=%d destroy=%d\n",
            received, direct_received, error, disabled, destroyed);
    return error || disabled || destroyed || !(direct_path ? direct_received : received) ? 6 : 0;
}
