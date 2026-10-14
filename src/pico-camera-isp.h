#ifndef REFRAMED_PICO_CAMERA_ISP_H
#define REFRAMED_PICO_CAMERA_ISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_CAMERA_IFE_ACQUIRE_SIZE 264

enum pico_camera_raw_format {
    PICO_CAMERA_RAW_PLAIN8,
    PICO_CAMERA_RAW_PLAIN16,
    PICO_CAMERA_RAW_MIPI,
};

struct pico_camera_ife_config {
    uint32_t phy, cphy, lanes, lane_map;
    uint32_t virtual_channel, data_type, bits;
    uint32_t width, height;
    enum pico_camera_raw_format output;
};

/* Encode one uncropped, nonsecure CSI input and one RDI0 output for the b1977
 * Spectra acquire ABI. All sensor/PHY settings must come from the selected
 * hardware configuration. This validates representation, not physical wiring.
 * No resource is acquired and no device is opened. Failure preserves output. */
bool pico_camera_ife_acquire(const struct pico_camera_ife_config *config,
                             void *output, size_t size);

#endif
