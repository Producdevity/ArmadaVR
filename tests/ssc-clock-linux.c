#include "ssc-clock-linux.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(__linux__) && defined(__aarch64__)
enum mode { REAL, FAIL, BACKWARD, WIDE, NANO, OVERFLOW, BEST };
static enum mode mode;
static unsigned reads;
int __real_clock_gettime(clockid_t clock, struct timespec *time);
int __wrap_clock_gettime(clockid_t clock, struct timespec *time)
{
    assert(clock == CLOCK_MONOTONIC);
    unsigned index = reads++;
    if (mode == REAL)
        return __real_clock_gettime(clock, time);
    if (mode == FAIL) {
        errno = EINVAL;
        return -1;
    }
    *time = (struct timespec){.tv_sec = 5, .tv_nsec = 1000};
    if (mode == BACKWARD)
        time->tv_nsec -= index % 2;
    else if (mode == WIDE)
        time->tv_nsec += (index % 2) * 10000;
    else if (mode == NANO)
        time->tv_nsec = 1000000000;
    else if (mode == OVERFLOW)
        time->tv_sec = INT64_MAX;
    else if (mode == BEST) {
        const long spans[] = {50, 3, 20, 10000};
        time->tv_nsec += (index % 2) * spans[index / 2];
    }
    return 0;
}

static int64_t now_ns(void)
{
    struct timespec now;
    assert(!__real_clock_gettime(CLOCK_MONOTONIC, &now));
    return now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}
#endif

int main(void)
{
    struct ssc_clock original, clock;
    memset(&original, 0x5a, sizeof(original));
    clock = original;
    assert(!ssc_clock_calibrate_monotonic(NULL, 1, 1000));
    assert(!ssc_clock_calibrate_monotonic(&clock, 0, 1000));
    assert(!ssc_clock_calibrate_monotonic(&clock, 65, 1000));
    assert(!ssc_clock_calibrate_monotonic(&clock, 1, -1));
    assert(!memcmp(&clock, &original, sizeof(clock)));
#if defined(__linux__) && defined(__aarch64__)
    assert(!reads);
    for (mode = FAIL; mode <= OVERFLOW; mode++) {
        reads = 0;
        assert(!ssc_clock_calibrate_monotonic(&clock, 4, 100));
        assert(!memcmp(&clock, &original, sizeof(clock)));
    }
    mode = BEST;
    reads = 0;
    assert(ssc_clock_calibrate_monotonic(&clock, 4, 100));
    assert(reads == 8 && clock.host_ns == INT64_C(5000001001));
    int64_t quantum = (1000000000 + (uint64_t)clock.counter_hz - 1) / clock.counter_hz;
    assert(clock.uncertainty_ns == 4 + quantum);
    mode = REAL;
    for (unsigned i = 0; i < 64; i++) {
        assert(ssc_clock_calibrate_monotonic(&clock, 16, 1000000));
        int64_t before = now_ns();
        uint64_t counter;
        __asm__ volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(counter) : : "memory");
        int64_t after = now_ns();
        __uint128_t scaled = (__uint128_t)counter * 19200000 / clock.counter_hz;
        assert(scaled <= UINT64_MAX);
        struct timespec delay = {.tv_nsec = 2000000};
        assert(!nanosleep(&delay, NULL));
        struct ssc_timestamp mapped;
        assert(ssc_clock_map(&clock, (uint64_t)scaled, now_ns(), 1000000000, &mapped));
        assert(mapped.timestamp_ns + mapped.uncertainty_ns + 54 >= before);
        assert(mapped.timestamp_ns - mapped.uncertainty_ns - 54 <= after);
    }
    printf("SSC_CLOCK_CAPTURE_PASS cycles=64 frequency=%u uncertainty=%lld\n",
           clock.counter_hz, (long long)clock.uncertainty_ns);
#else
    assert(!ssc_clock_calibrate_monotonic(&clock, 1, 1000));
    assert(!memcmp(&clock, &original, sizeof(clock)));
    puts("SSC_CLOCK_CAPTURE_UNSUPPORTED_PASS");
#endif
    return 0;
}
