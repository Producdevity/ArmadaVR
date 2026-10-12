#include "ssc-clock.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void rejected(const struct ssc_clock *clock, uint64_t ticks,
                     int64_t now, int64_t age)
{
    struct ssc_timestamp result = {123, 456}, original = result;
    assert(!ssc_clock_map(clock, ticks, now, age, &result));
    assert(!memcmp(&result, &original, sizeof(result)));
}

int main(void)
{
    struct ssc_clock clock = {0};
    struct ssc_timestamp result;
    rejected(&clock, 0, 0, 0);
    assert(!ssc_clock_init(NULL, 0, 19200000, 0, 0, 0));
    assert(ssc_clock_init(&clock, 19200000, 19200000, 1000000000, 1000000101, 101));
    assert(clock.host_ns == 1000000050 && clock.uncertainty_ns == 53);
    struct ssc_clock saved = clock;
    assert(!ssc_clock_init(&clock, 0, 0, 0, 0, 0));
    assert(!ssc_clock_init(&clock, 0, 1, -1, 0, 1));
    assert(!ssc_clock_init(&clock, 0, 1, 1, 0, 1));
    assert(!ssc_clock_init(&clock, 0, 1, 0, 1, 0));
    assert(!ssc_clock_init(&clock, 0, 1, 0, 0, -1));
    assert(!memcmp(&clock, &saved, sizeof(clock)));
    assert(ssc_clock_map(&clock, 19200000, 1000000103, 106, &result));
    assert(result.timestamp_ns == 1000000050 && result.uncertainty_ns == 53);
    rejected(&clock, 19200000, 1000000102, 106);
    rejected(&clock, 19200000, 1000000103, 105);
    rejected(&clock, UINT64_MAX, INT64_MAX, INT64_MAX);
    rejected(&clock, 0, INT64_MAX, INT64_MAX);
    rejected(&clock, 19200000, -1, 1);
    rejected(&clock, 19200000, 1000000103, -1);
    rejected(NULL, 1, 1, 1);
    assert(!ssc_clock_map(&clock, 19200000, 1000000103, 106, NULL));
    assert(!memcmp(&clock, &saved, sizeof(clock)));

    /* CPU and DSP counters need not advertise the same frequency. */
    assert(ssc_clock_init(&clock, 24000000, 24000000, 2000000000, 2000000000, 0));
    assert(ssc_clock_map(&clock, 19200000, 2000000002, 4, &result));
    assert(result.timestamp_ns == 2000000000 && result.uncertainty_ns == 2);
    assert(ssc_clock_map(&clock, 9600000, 2000000000, 500000002, &result));
    assert(result.timestamp_ns == 1500000000);

    /* Preserve small tick deltas even above double's integer precision. */
    uint64_t anchor = UINT64_C(0x123456789abcdef);
    assert(ssc_clock_init(&clock, anchor, 19200000, 1000000000, 1000000000, 0));
    for (uint64_t i = 1; i < 10000; ++i) {
        int64_t delta = (int64_t)((anchor % 12 + i) * 625 / 12 - (anchor % 12) * 625 / 12);
        assert(ssc_clock_map(&clock, anchor + i, 2000000000, 2000000000, &result));
        assert(result.timestamp_ns == 1000000000 + delta);
    }
    assert(ssc_clock_init(&clock, UINT64_MAX, 19200000, 1000000000, 1000000000, 0));
    assert(ssc_clock_map(&clock, UINT64_MAX, 1000000002, 4, &result));
    assert(result.timestamp_ns == 1000000000);
    rejected(&clock, 0, 1000000002, INT64_MAX);
    assert(ssc_clock_init(&clock, 0, UINT32_MAX, INT64_MAX, INT64_MAX, 0));
    rejected(&clock, 1, INT64_MAX, INT64_MAX);
    assert(ssc_clock_init(&clock, UINT64_MAX, 1, 0, INT64_MAX, INT64_MAX));
    rejected(&clock, UINT64_MAX, INT64_MAX, INT64_MAX);
    puts("SSC clock alignment and rejection checks passed");
    return 0;
}
