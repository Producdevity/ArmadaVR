#define _POSIX_C_SOURCE 200809L
#include <android/hardware_buffer.h>
#include <android/sensor.h>
#include <sys/system_properties.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CAPACITY 8192
static volatile sig_atomic_t interrupted;

static void stop(int signal_number)
{
    if (signal_number == SIGALRM)
        _exit(124);
    interrupted = 1;
}

static int64_t clock_ns(clockid_t id)
{
    struct timespec time;
    if (clock_gettime(id, &time) || time.tv_sec < 0 || time.tv_nsec < 0 ||
        time.tv_nsec >= 1000000000)
        return -1;
    __int128 ns = (__int128)time.tv_sec * 1000000000 + time.tv_nsec;
    return ns > INT64_MAX ? -1 : (int64_t)ns;
}

static bool clocks(const char *phase)
{
    const clockid_t ids[] = {CLOCK_MONOTONIC, CLOCK_BOOTTIME};
    const char *names[] = {"monotonic", "boottime"};
    for (unsigned clock = 0; clock < 2; clock++) {
        for (unsigned attempt = 0; attempt < 16; attempt++) {
            uint64_t ticks, frequency, after_frequency;
            __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
            int64_t before = clock_ns(ids[clock]);
            __asm__ volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(ticks) : : "memory");
            int64_t after = clock_ns(ids[clock]);
            __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(after_frequency));
            if (before < 0 || after < before || !frequency || frequency != after_frequency)
                return false;
            printf("CLOCK %s %s %" PRId64 " %" PRIu64 " %" PRIu64 " %" PRId64 "\n",
                   phase, names[clock], before, ticks, frequency, after);
        }
    }
    return true;
}

static int order(const void *left, const void *right)
{
    uint32_t a = ((const ASensorEvent *)left)->reserved0;
    uint32_t b = ((const ASensorEvent *)right)->reserved0;
    return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--capture"))
        return 2;
    char identity[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.build.fingerprint", identity);
    if (strcmp(identity, "Pico/A7H10/PICOA7H10:10/5.8.4.0/smartcm.1696861506:user/test-keys"))
        return 2;
    _Static_assert(sizeof(ASensorEvent) == 104, "Unexpected Android sensor layout");
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGALRM, stop);
    alarm(8);
    setvbuf(stdout, NULL, _IOLBF, 0);
    ASensorManager *manager = ASensorManager_getInstanceForPackage("com.android.shell");
    if (!manager)
        return 1;
    ASensorList list = NULL;
    int count = ASensorManager_getSensorList(manager, &list);
    if (count <= 0 || count > 1024 || !list)
        return 1;
    printf("PICO_IMU_CAPTURE 1\n");
    const int types[2] = {35, 16};
    const ASensor *sensors[2] = {0};
    int rates[2] = {0}, tokens[2] = {0}, result = 1, channel = 0;
    AHardwareBuffer *buffer = NULL;
    ASensorEvent *events = NULL;
    void *memory = NULL;
    bool locked = false;
    size_t size = CAPACITY * sizeof(ASensorEvent);
    for (unsigned i = 0; i < 2; i++) {
        sensors[i] = ASensorManager_getDefaultSensor(manager, types[i]);
        if (!sensors[i]) {
            for (int j = 0; j < count; j++) {
                if (ASensor_getType(list[j]) != types[i] || ASensor_isWakeUpSensor(list[j]))
                    continue;
                if (sensors[i])
                    goto cleanup;
                sensors[i] = list[j];
            }
        }
        if (!sensors[i] || ASensor_getType(sensors[i]) != types[i] ||
            ASensor_isWakeUpSensor(sensors[i]))
            goto cleanup;
        bool direct = ASensor_isDirectChannelTypeSupported(
            sensors[i], ASENSOR_DIRECT_CHANNEL_TYPE_HARDWARE_BUFFER);
        rates[i] = ASensor_getHighestDirectReportRateLevel(sensors[i]);
        printf("SENSOR %d %s direct=%d max_rate_level=%d\n",
               types[i], ASensor_getName(sensors[i]), direct, rates[i]);
        if (!direct || rates[i] < ASENSOR_DIRECT_RATE_NORMAL ||
            rates[i] > ASENSOR_DIRECT_RATE_VERY_FAST)
            goto cleanup;
    }
    AHardwareBuffer_Desc desc = {
        .width = size, .height = 1, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_BLOB,
        .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                 AHARDWAREBUFFER_USAGE_SENSOR_DIRECT_DATA | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER,
    };
    if (AHardwareBuffer_allocate(&desc, &buffer))
        goto cleanup;
    channel = ASensorManager_createHardwareBufferDirectChannel(manager, buffer, size);
    if (channel <= 0)
        goto cleanup;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &memory))
        goto cleanup;
    locked = true;
    if (!clocks("before"))
        goto cleanup;
    for (unsigned i = 0; i < 2; i++) {
        tokens[i] = ASensorManager_configureDirectReport(manager, sensors[i], channel, rates[i]);
        printf("TOKEN %d %d\n", types[i], tokens[i]);
        if (tokens[i] <= 0)
            goto cleanup;
    }
    int64_t started = clock_ns(CLOCK_MONOTONIC), now;
    if (started < 0)
        goto cleanup;
    while (!interrupted) {
        now = clock_ns(CLOCK_MONOTONIC);
        if (now < started)
            goto cleanup;
        if (now - started >= INT64_C(2000000000))
            break;
        struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    if (ASensorManager_configureDirectReport(manager, NULL, channel, ASENSOR_DIRECT_RATE_STOP) < 0 ||
        interrupted || !clocks("after"))
        goto cleanup;
    events = calloc(CAPACITY, sizeof(*events));
    if (!events)
        goto cleanup;
    unsigned total = 0, counts[2] = {0};
    for (size_t slot = 0; slot < CAPACITY; slot++) {
        ASensorEvent *entry = (ASensorEvent *)memory + slot, event;
        uint32_t sequence = __atomic_load_n((uint32_t *)&entry->reserved0, __ATOMIC_ACQUIRE);
        if (!sequence)
            continue;
        memcpy(&event, entry, sizeof(event));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (sequence != __atomic_load_n((uint32_t *)&entry->reserved0, __ATOMIC_ACQUIRE) ||
            sequence != (uint32_t)event.reserved0 || event.version != sizeof(event) || event.timestamp <= 0)
            goto cleanup;
        unsigned index = event.type == types[0] ? 0 : 1;
        if (event.type != types[index] || event.sensor != tokens[index])
            goto cleanup;
        for (unsigned j = 0; j < 6; j++)
            if (!isfinite(event.data[j]))
                goto cleanup;
        events[total++] = event;
        counts[index]++;
    }
    qsort(events, total, sizeof(*events), order);
    for (unsigned i = 0; i < total; i++) {
        ASensorEvent *event = events + i;
        if ((uint32_t)event->reserved0 != i + 1)
            goto cleanup;
        printf("SAMPLE %u %d %" PRId64 " %.9g %.9g %.9g %.9g %.9g %.9g\n",
               event->reserved0, event->type, event->timestamp, event->data[0], event->data[1],
               event->data[2], event->data[3], event->data[4], event->data[5]);
    }
    if (counts[0] > 10 && counts[1] > 10) {
        printf("COUNTS %u %u\n", counts[0], counts[1]);
        result = 0;
    }
cleanup:
    if (channel > 0) {
        int stopped = ASensorManager_configureDirectReport(manager, NULL, channel, ASENSOR_DIRECT_RATE_STOP);
        ASensorManager_destroyDirectChannel(manager, channel);
        if (stopped < 0)
            result = 1;
    }
    if (locked && AHardwareBuffer_unlock(buffer, NULL))
        result = 1;
    if (buffer)
        AHardwareBuffer_release(buffer);
    free(events);
    alarm(0);
    if (!result && !interrupted)
        puts("PICO_IMU_CAPTURE_PASS");
    else
        result = 1;
    return result;
}
