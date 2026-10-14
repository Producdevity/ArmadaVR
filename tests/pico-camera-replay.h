#ifndef PICO_CAMERA_REPLAY_H
#define PICO_CAMERA_REPLAY_H
#include "pico-camera-transport.h"
void pico_camera_replay_config(struct pico_camera_transport_config *config,
                              uint32_t width, uint32_t height, const uint8_t camera_ids[2]);
void pico_camera_replay_feed(unsigned camera, const uint8_t *pixels, size_t stride, int64_t qtimer_ns);
unsigned pico_camera_replay_finish(void);
#endif
