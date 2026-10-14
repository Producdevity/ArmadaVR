#ifndef REFRAMED_PICO_CAMERA_ISP_H
#define REFRAMED_PICO_CAMERA_ISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_CAMERA_IFE_ACQUIRE_SIZE 264
#define PICO_CAMERA_CSIPHY_SIZE 32

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

struct pico_camera_csiphy_config {
    uint32_t cphy, combo, lanes, lane_map;
    uint32_t settle_count;
    uint64_t data_rate;
    uint32_t mipi_flags;
};

/* Encode the nonsecure b1977 PHY payload using an explicit per-lane data rate
 * and sensor settle register count. The kernel divides the wire settle value
 * by 200000000; its UAPI comment calling the value milliseconds is inaccurate.
 * Active lane nibbles must be distinct and valid for the selected PHY mode.
 * No hardware is configured. Failure preserves output. */
bool pico_camera_csiphy_configure(const struct pico_camera_csiphy_config *config,
                                  void *output, size_t size);

#endif
