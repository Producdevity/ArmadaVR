#ifndef REFRAMED_PICO_CAMERA_IFE_H
#define REFRAMED_PICO_CAMERA_IFE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_CAMERA_IFE_RDI_COMMAND_SIZE 28
struct pico_camera_ife_rdi {
    bool lite;
    uint32_t index, sample_pattern, batch_size;
};
/* b1977 Titan480 CAMIF commands for an explicitly acquired RDI path. Full IFE
 * has RDI0..2; Lite has RDI0..3. Pattern is the hardware sample-pattern value,
 * not a CSI data type. The caller must match batch size to buffer/HFR settings.
 * No pixel/line subsampling, sensor selection, I/O or MMIO is performed. */
bool pico_camera_ife_rdi_commands(const struct pico_camera_ife_rdi *config,
                                  void *output, size_t size);

#define PICO_CAMERA_IFE_THROTTLE_COMMAND_SIZE 12
/* Explicit finite utilization 0..1, using stock's four throttle intervals.
 * Only for the hardware path that owns the Titan480 throttle register. */
bool pico_camera_ife_throttle_command(float utilization, void *output, size_t size);
#endif
