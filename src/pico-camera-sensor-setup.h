#ifndef REFRAMED_PICO_CAMERA_SENSOR_SETUP_H
#define REFRAMED_PICO_CAMERA_SENSOR_SETUP_H
#include "pico-camera-sensor.h"

struct pico_camera_sensor_setup;
struct pico_camera_sensor_table {
    struct pico_camera_sensor_bus bus;
    const struct pico_camera_sensor_write *writes;
    size_t count;
};
struct pico_camera_sensor_setup_config {
    int crm_fd, sensor_fd;
    int32_t session, sensor;
    struct pico_camera_sensor_table initialize, mode, synchronize, stream_on, stream_off;
};

/* Copies explicit sensor tables into owned CRM command/packet allocations.
 * The sensor must already be probed and acquired. Descriptors are borrowed;
 * table storage is borrowed only during this call. Only synchronize may be empty.
 * No module, mode, synchronization role or physical wiring is inferred.
 * A nonnull owner must be released even when preparation fails. */
int pico_camera_sensor_setup_prepare(const struct pico_camera_sensor_setup_config *config,
                                     struct pico_camera_sensor_setup **setup);
/* One attempt: initialize, mode, optional sync settings, stream-on/off tables.
 * This configures registers but does not start the sensor. Failure may leave
 * partially applied settings and must not be retried on the same acquisition. */
int pico_camera_sensor_setup_submit(struct pico_camera_sensor_setup *setup);
/* Call only after all control operations have returned and independent device
 * quiescence is established. Success/timeout/flush alone is not that evidence. */
int pico_camera_sensor_setup_quiesced(struct pico_camera_sensor_setup *setup);
/* Keeps attempted command allocations until quiesced, including after errors. */
int pico_camera_sensor_setup_release(struct pico_camera_sensor_setup **setup);
#endif
