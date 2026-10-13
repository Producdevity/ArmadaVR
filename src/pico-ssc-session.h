#ifndef REFRAMED_PICO_SSC_SESSION_H
#define REFRAMED_PICO_SSC_SESSION_H

#include "pico-monado-device.h"

struct pico_ssc_session;

struct pico_ssc_session_config {
    bool (*calibrate)(void *data, struct ssc_clock *clock);
    void *clock_data;
    int64_t max_age_ns, timeout_ns;
    unsigned mcu_type, radio_type;
    bool touch_supported;
    const struct pico_analog_calibration *left, *right;
};

struct pico_ssc_session_status {
    uint64_t accepted, rejected;
    bool streaming, stopping, finished, failed;
    char error[192];
};

/* Opens ble_mcu on a private worker. Calibration runs there after discovery and
 * must map to CLOCK_MONOTONIC; clock_data must live until destroy returns.
 * Configuration and analog calibration are copied. There is no automatic retry:
 * a new session must establish a fresh clock after resume or DSP restart.
 * Output pointers must initially be NULL. Devices have independent lifetimes. */
bool pico_ssc_session_create(const struct pico_ssc_session_config *config,
                             struct pico_ssc_session **session,
                             struct xrt_device **left, struct xrt_device **right);
void pico_ssc_session_get_status(struct pico_ssc_session *session,
                                 struct pico_ssc_session_status *status);
void pico_ssc_session_stop(struct pico_ssc_session *session);
/* Cancels discovery/subscription, disables the sensor and joins transport
 * cleanup. Call on the owning runtime thread, never from calibrate. Do not race
 * destroy with other public session calls. Devices may outlive the session. */
void pico_ssc_session_destroy(struct pico_ssc_session **session);

#endif
