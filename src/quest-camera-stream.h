#ifndef REFRAMED_QUEST_CAMERA_STREAM_H
#define REFRAMED_QUEST_CAMERA_STREAM_H
#include "quest-camera-shutter.h"

struct quest_camera_stream;
struct quest_camera_stream_config {
    uint64_t generation;
    uint32_t camera_bits[4];
    size_t camera_count;
    int64_t silence_ns;
    uint64_t max_gap_us;
    void *userdata;
    bool (*shutter)(void *userdata, uint64_t generation, uint32_t camera_bit,
                    const struct quest_camera_shutter *shutter, int64_t received_ns);
    void (*invalidated)(void *userdata, uint64_t generation, int error);
};

/* Opens only the copied Syncboss stream FIFO, selects packet 254 and clears this
 * client's queue. Explicitly select 1..4 cameras and verified gap limits. This
 * does not enable/configure the MCU or start capture. Calls are serialized and
 * callbacks must not reenter. Stop on MCU restart, suspend or ownership loss. */
bool quest_camera_stream_open(const struct quest_camera_stream_config *config,
                              struct quest_camera_stream **stream);
/* At most eight 4 KiB reads. Validate each complete batch before publishing any
 * shutter. Selected cameras have independent silence and raw timestamp checks.
 * Return delivered records or negative errno. Raw MCU times remain unconverted;
 * received_ns measures FIFO observation, not exposure or capture freshness.
 * The consumer must establish the camera/IMU clock relationship independently
 * and reject stale shutters when matching them to captured images. */
int quest_camera_stream_poll(struct quest_camera_stream *stream);
void quest_camera_stream_stop(struct quest_camera_stream *stream);
/* Closing this copied FIFO only releases this client's queue. It does not prove
 * MCU shutdown or camera DMA quiescence. A failed owner never restarts. */
void quest_camera_stream_destroy(struct quest_camera_stream **stream);
#endif
