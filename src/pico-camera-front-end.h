#ifndef REFRAMED_PICO_CAMERA_FRONT_END_H
#define REFRAMED_PICO_CAMERA_FRONT_END_H
#include "pico-camera-control.h"
#include "pico-camera-isp.h"

struct pico_camera_front_end;
struct pico_camera_front_end_target {
    int crm_fd, device_fd;
    int32_t session, device;
};
struct pico_camera_ife_setup {
    struct pico_camera_front_end_target target;
    struct pico_camera_ife_hardware hardware;
    int32_t command_mmu;
    enum pico_camera_memory_mapping command_mapping;
    uint32_t sample_pattern;
    struct pico_camera_ife_clocks clocks;
    struct pico_camera_ife_bandwidth bandwidth;
    bool configure_throttle;
    float utilization;
};

/* Prepare owned commands for one acquired Titan480 RDI0 input, without crop,
 * batching or pixel/line subsampling. The acquisition result must identify one
 * full/Lite IFE and only the left RDI0 path. Clocks, bandwidth, sample pattern,
 * MMU and optional throttle ownership come from the selected configuration.
 * No device is opened, acquired or started. Input storage is copied. */
int pico_camera_front_end_prepare_ife(const struct pico_camera_ife_setup *config,
                                     struct pico_camera_front_end **setup);
int pico_camera_front_end_prepare_phy(const struct pico_camera_front_end_target *target,
                                     const struct pico_camera_csiphy_config *config,
                                     struct pico_camera_front_end **setup);
/* One configuration attempt. Failed preparation can still return an owner;
 * failed submission retains all attempted buffers until verified quiescence. */
int pico_camera_front_end_submit(struct pico_camera_front_end *setup);
/* Requires returned control calls and independent device/DMA quiescence;
 * configuration success, timeout or flush alone is not sufficient. */
int pico_camera_front_end_quiesced(struct pico_camera_front_end *setup);
int pico_camera_front_end_release(struct pico_camera_front_end **setup);
#endif
