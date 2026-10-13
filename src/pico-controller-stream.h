#ifndef REFRAMED_PICO_CONTROLLER_STREAM_H
#define REFRAMED_PICO_CONTROLLER_STREAM_H

#include "pico-controller.h"
#include "ssc-clock.h"

struct pico_controller_stream_config {
    struct ssc_clock clock;
    uint64_t suid_high, suid_low;
    int64_t max_age_ns;
    unsigned mcu_type, radio_type;
    bool touch_supported;
};

struct pico_controller_event {
    struct pico_controller_sample sample;
    struct ssc_timestamp time;
    unsigned accuracy;
    uint64_t generation;
};

struct pico_controller_stream {
    struct pico_controller_stream_config config;
    uint64_t generation, last_ticks[2];
    int64_t last_time_ns[2], last_received_ns;
    bool active, seen[2];
};

/* Zero-initialize once; confine calls to one thread. Each successful begin
 * returns a new generation in stream->generation. Capture that value in the
 * subscription callback, so queued reports from an old connection are refused.
 * Use a newly calibrated clock after reconnect, resume or DSP restart. */
bool pico_controller_stream_begin(struct pico_controller_stream *stream,
                                  const struct pico_controller_stream_config *config);

/* Stop accepting samples immediately. Consumers must release active controls
 * when disconnecting; invalidation does not generate artificial input events. */
void pico_controller_stream_invalidate(struct pico_controller_stream *stream);

/* Require matching subscription, SUID and message 1024; decode, map time and
 * enforce strictly increasing timestamps separately for each hand. now_ns uses
 * the calibration's host clock. No payload pointer is retained. Rejection
 * preserves stream and output; this does not open a transport or produce pose. */
bool pico_controller_stream_report(struct pico_controller_stream *stream, uint64_t generation,
                                   uint32_t id, uint64_t high, uint64_t low, uint64_t ticks,
                                   const void *payload, size_t length, int64_t now_ns,
                                   struct pico_controller_event *event);

#endif
