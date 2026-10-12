#ifndef ARMADA_SSC_CLOCK_H
#define ARMADA_SSC_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

struct ssc_clock {
    uint64_t counter_ticks;
    uint32_t counter_hz;
    int64_t host_ns, uncertainty_ns;
    bool initialized;
};

struct ssc_timestamp {
    int64_t timestamp_ns, uncertainty_ns;
};

/* Bracket a CPU QTimer read with two reads of the chosen host clock. SSC uses
 * 19.2 MHz ticks; counter_hz is the CPU's measured CNTFRQ. Reject wide brackets.
 * Recalibrate and reset stream ordering after resume, DSP reset or reconnect.
 * This does not read hardware or establish shared clock origin on a device. */
bool ssc_clock_init(struct ssc_clock *clock, uint64_t counter_ticks,
                    uint32_t counter_hz, int64_t before_ns, int64_t after_ns,
                    int64_t max_span_ns);

/* now_ns uses the calibration's host clock. Accept only when the entire
 * uncertainty interval is nonnegative, not in the future, and within max_age.
 * Mapping is stateless; the caller enforces stream identity and ordering.
 * Rejection preserves state and output. */
bool ssc_clock_map(const struct ssc_clock *clock, uint64_t ticks,
                   int64_t now_ns, int64_t max_age_ns,
                   struct ssc_timestamp *timestamp);

#endif
