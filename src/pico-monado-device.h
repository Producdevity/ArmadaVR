#ifndef REFRAMED_PICO_MONADO_DEVICE_H
#define REFRAMED_PICO_MONADO_DEVICE_H

#include "pico-monado-input.h"

struct pico_monado_producer;

/* The producer and both devices own independent references. Stop and join the
 * transport before releasing its producer; devices may be destroyed separately.
 * Monado owns update_inputs and its public input arrays. All event timestamps
 * must be calibrated to CLOCK_MONOTONIC, as required by xrt_device. Output
 * pointers must initially be NULL; failed creation leaves them unchanged. */
bool pico_monado_devices_create(int64_t timeout_ns,
                                struct pico_monado_producer **producer,
                                struct xrt_device **left, struct xrt_device **right);

/* A transport reconnect requires a strictly newer subscription generation.
 * Calibration pointers may be NULL; their contents are copied. */
bool pico_monado_producer_begin(struct pico_monado_producer *producer,
                                uint64_t generation, bool touch_supported,
                                const struct pico_analog_calibration *left,
                                const struct pico_analog_calibration *right);
/* Accept only events already validated by pico_controller_stream_report. */
bool pico_monado_producer_push(struct pico_monado_producer *producer,
                               const struct pico_controller_event *event);
void pico_monado_producer_disconnect(struct pico_monado_producer *producer);
void pico_monado_producer_destroy(struct pico_monado_producer **producer);

#endif
