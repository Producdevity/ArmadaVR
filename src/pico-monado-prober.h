#ifndef REFRAMED_PICO_MONADO_PROBER_H
#define REFRAMED_PICO_MONADO_PROBER_H

struct xrt_auto_prober;

/* Inactive unless PICO_SSC_CONFIG names a stock configuration receipt and
 * PICO_SSC_COUNTER=1 explicitly permits userspace ARM counter calibration.
 * The prober owns the SSC session; controller devices may outlive it. */
struct xrt_auto_prober *pico_ssc_create_auto_prober(void);

#endif
