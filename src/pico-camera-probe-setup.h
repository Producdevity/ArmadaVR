#ifndef REFRAMED_PICO_CAMERA_PROBE_SETUP_H
#define REFRAMED_PICO_CAMERA_PROBE_SETUP_H
#include "pico-camera-sensor.h"

struct pico_camera_probe_setup;
struct pico_camera_probe_setup_config {
    int crm_fd, sensor_fd;
    struct pico_camera_sensor_bus bus;
    struct pico_camera_sensor_probe identity;
    const struct pico_camera_sensor_power_step *power_up, *power_down;
    size_t power_up_count, power_down_count;
};

/* Copies explicit identity and power sequences into owned CRM buffers. Fds are
 * borrowed; sequence storage is borrowed only during preparation. A nonnull
 * owner must be released even when preparation fails. No hardware is selected. */
int pico_camera_probe_setup_prepare(const struct pico_camera_probe_setup_config *config,
                                    struct pico_camera_probe_setup **setup);
/* One attempt. Probing powers the sensor and may fail after changing hardware.
 * A failed probe must not be retried on this owner. Calls are serialized. */
int pico_camera_probe_setup_submit(struct pico_camera_probe_setup *setup);
/* Requires all control calls to have returned and independent device quiescence.
 * A successful probe or flush alone does not establish that condition. */
int pico_camera_probe_setup_quiesced(struct pico_camera_probe_setup *setup);
int pico_camera_probe_setup_release(struct pico_camera_probe_setup **setup);
#endif
