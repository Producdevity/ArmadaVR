#include "ssc-clock.h"

#include <limits.h>

bool ssc_clock_init(struct ssc_clock *clock, uint64_t counter_ticks,
                    uint32_t counter_hz, int64_t before_ns, int64_t after_ns,
                    int64_t max_span_ns)
{
    if (!clock || !counter_hz || before_ns < 0 || after_ns < before_ns ||
        max_span_ns < 0 || after_ns - before_ns > max_span_ns)
        return false;
    int64_t span = after_ns - before_ns;
    *clock = (struct ssc_clock){
        .counter_ticks = counter_ticks,
        .counter_hz = counter_hz,
        .host_ns = before_ns + span / 2,
        .uncertainty_ns = span / 2 + span % 2 + 2,
        .initialized = true,
    };
    return true;
}

bool ssc_clock_map(const struct ssc_clock *clock, uint64_t ticks,
                   int64_t now_ns, int64_t max_age_ns,
                   struct ssc_timestamp *timestamp)
{
    if (!clock || !timestamp || !clock->initialized || !clock->counter_hz ||
        clock->host_ns < 0 || clock->uncertainty_ns < 2 || now_ns < 0 || max_age_ns < 0)
        return false;
    __int128 event_ns = (__int128)ticks * 1000000000 / 19200000;
    __int128 counter_ns = (__int128)clock->counter_ticks * 1000000000 / clock->counter_hz;
    __int128 mapped = clock->host_ns + event_ns - counter_ns;
    __int128 earliest = mapped - clock->uncertainty_ns;
    __int128 latest = mapped + clock->uncertainty_ns;
    if (earliest < 0 || latest > now_ns || (__int128)now_ns - earliest > max_age_ns)
        return false;
    *timestamp = (struct ssc_timestamp){
        .timestamp_ns = (int64_t)mapped,
        .uncertainty_ns = clock->uncertainty_ns,
    };
    return true;
}
