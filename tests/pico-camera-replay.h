#ifndef PICO_CAMERA_REPLAY_H
#define PICO_CAMERA_REPLAY_H
#include "pico-camera-transport.h"
#include "pico-slam-live.h"
void pico_camera_replay_config(struct pico_camera_transport_config *config,
                              uint32_t width, uint32_t height, const uint8_t camera_ids[2]);
void pico_camera_replay_feed(unsigned camera, const uint8_t *pixels, size_t stride, int64_t qtimer_ns);
unsigned pico_camera_replay_finish(void);
void pico_camera_replay_live_config(struct pico_slam_live_config *config, uint32_t width, uint32_t height, const uint8_t ids[2]);
void pico_camera_replay_imu(const struct xrt_imu_sample *sample, uint64_t ticks, int64_t now);
void pico_camera_replay_clock(int64_t now);
#endif
