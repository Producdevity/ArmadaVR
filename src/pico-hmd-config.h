#ifndef REFRAMED_PICO_HMD_CONFIG_H
#define REFRAMED_PICO_HMD_CONFIG_H

#include "pico-monado-hmd.h"

/* Read a bounded private calibration file. No sensor access; the returned
 * configuration owns its serial and distortion grids. */
bool pico_hmd_config_load(const char *path, struct pico_hmd_config **config);
void pico_hmd_config_destroy(struct pico_hmd_config **config);

#endif
