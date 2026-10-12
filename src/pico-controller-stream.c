#include "pico-controller-stream.h"

#include <limits.h>

bool pico_controller_stream_begin(struct pico_controller_stream *stream,
                                  const struct pico_controller_stream_config *config)
{
    if (!stream || !config || stream->generation == UINT64_MAX ||
        (!config->suid_high && !config->suid_low) || config->max_age_ns < 0 ||
        !config->clock.initialized || !config->clock.counter_hz ||
        config->clock.host_ns < 0 || config->clock.uncertainty_ns < 2 ||
        config->mcu_type < 1 || config->mcu_type > 2 ||
        config->radio_type < 1 || config->radio_type > 2)
        return false;
    *stream = (struct pico_controller_stream){
        .config = *config,
        .generation = stream->generation + 1,
        .active = true,
    };
    return true;
}

void pico_controller_stream_invalidate(struct pico_controller_stream *stream)
{
    if (stream)
        stream->active = false;
}

bool pico_controller_stream_report(struct pico_controller_stream *stream, uint64_t generation,
                                   uint32_t id, uint64_t high, uint64_t low, uint64_t ticks,
                                   const void *payload, size_t length, int64_t now_ns,
                                   struct pico_controller_event *event)
{
    if (!stream || !event || !stream->active || generation != stream->generation ||
        id != 1024 || high != stream->config.suid_high || low != stream->config.suid_low ||
        now_ns < stream->last_received_ns)
        return false;
    struct pico_controller_event decoded = {.generation = generation};
    if (!pico_controller_decode_ssc(payload, length, stream->config.mcu_type,
                                    stream->config.radio_type, stream->config.touch_supported,
                                    &decoded.sample, &decoded.accuracy) ||
        !ssc_clock_map(&stream->config.clock, ticks, now_ns, stream->config.max_age_ns, &decoded.time))
        return false;
    unsigned hand = decoded.sample.hand - 1;
    if (hand >= 2 || (stream->seen[hand] &&
        (ticks <= stream->last_ticks[hand] || decoded.time.timestamp_ns <= stream->last_time_ns[hand])))
        return false;
    stream->seen[hand] = true;
    stream->last_ticks[hand] = ticks;
    stream->last_time_ns[hand] = decoded.time.timestamp_ns;
    stream->last_received_ns = now_ns;
    *event = decoded;
    return true;
}
