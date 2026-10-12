#ifndef ARMADA_SSC_CLOCK_LINUX_H
#define ARMADA_SSC_CLOCK_LINUX_H

#include "ssc-clock.h"

/* Opt-in Linux ARM64 acquisition using CNTVCT_EL0/CNTFRQ_EL0 and
 * CLOCK_MONOTONIC. The kernel must permit userspace counter access. Keep the
 * narrowest of 1..64 brackets within max_span_ns, including counter resolution
 * in the uncertainty. Failure preserves output; other platforms return false.
 * This does not establish the DSP's clock origin. Reacquire after resume/reset. */
bool ssc_clock_calibrate_monotonic(struct ssc_clock *clock, unsigned attempts,
                                   int64_t max_span_ns);

#endif
