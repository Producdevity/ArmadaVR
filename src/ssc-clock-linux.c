#include "ssc-clock-linux.h"

#include <limits.h>
#include <time.h>

#if defined(__linux__) && defined(__aarch64__)
static bool monotonic_ns(int64_t *value)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) || time.tv_sec < 0 ||
        time.tv_nsec < 0 || time.tv_nsec >= 1000000000)
        return false;
    __int128 ns = (__int128)time.tv_sec * 1000000000 + time.tv_nsec;
    if (ns > INT64_MAX)
        return false;
    *value = (int64_t)ns;
    return true;
}
#endif

bool ssc_clock_calibrate_monotonic(struct ssc_clock *clock, unsigned attempts,
                                   int64_t max_span_ns)
{
    if (!clock || attempts == 0 || attempts > 64 || max_span_ns < 0)
        return false;
#if defined(__linux__) && defined(__aarch64__)
    struct ssc_clock best = {0};
    for (unsigned i = 0; i < attempts; i++) {
        uint64_t ticks, frequency, after_frequency;
        int64_t before, after;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
        if (!frequency || frequency > UINT32_MAX || !monotonic_ns(&before))
            return false;
        __asm__ volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(ticks) : : "memory");
        if (!monotonic_ns(&after))
            return false;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(after_frequency));
        if (frequency != after_frequency || after < before)
            return false;
        struct ssc_clock sample;
        if (!ssc_clock_init(&sample, ticks, (uint32_t)frequency, before, after, max_span_ns))
            continue;
        int64_t quantum = (1000000000 + frequency - 1) / frequency;
        if (sample.uncertainty_ns > INT64_MAX - quantum)
            return false;
        sample.uncertainty_ns += quantum;
        if (!best.initialized || sample.uncertainty_ns <= best.uncertainty_ns)
            best = sample;
    }
    if (!best.initialized)
        return false;
    *clock = best;
    return true;
#else
    return false;
#endif
}
