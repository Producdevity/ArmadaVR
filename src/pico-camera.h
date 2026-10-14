#ifndef REFRAMED_PICO_CAMERA_H
#define REFRAMED_PICO_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_CAMERA_HEADER_SIZE 89
#define PICO_CAMERA_MONO8 0x123
#define PICO_CAMERA_MAX_PIXELS (4096U * 4096U)
#define PICO_CAMERA_MAX_SKEW_NS INT64_C(100000)

struct pico_camera_frame {
    uint8_t camera_id;
    uint32_t width, height, exposure_lines, gain, classification, extra;
    uint64_t timestamp, frame_number;
    int64_t qtimer_ns, center_ns, line_ns;
    const uint8_t *pixels;
    size_t size;
};

/* Parse an immutable snapshot of b1977 CameraManager's 89-byte header and
 * contiguous mono8 plane. The caller owns the snapshot throughout use of the
 * returned view. Shared-memory synchronization and copying precede this call.
 * Other formats, padded rows and additional planes are deliberately refused.
 * Reserved stride fields are not interpreted. Failure preserves the output. */
bool pico_camera_decode(const void *snapshot, size_t length, struct pico_camera_frame *frame);

/* b1977's camera correction, in its qtimer nanosecond domain. All timing
 * parameters must be established for the selected camera; none are defaults.
 * This does not convert qtimer to CLOCK_MONOTONIC or apply IMU time offsets. */
bool pico_camera_center(int64_t qtimer_ns, uint32_t exposure_lines,
                         int64_t line_ns, int64_t readout_ns, int64_t *center_ns);

struct pico_camera_frame_set {
    struct pico_camera_frame frames[4];
    size_t count;
    int64_t first_ns, last_ns;
};

/* Require two or four complete head images in the explicitly configured camera
 * order, within the selected skew (at most stock's 100 us bound). No missing
 * camera, zero capture time or controller frame is admitted. All frames must
 * share a clock domain and stream generation, established by the caller.
 * Neither camera identities nor a calibration are selected by this function. */
bool pico_camera_head_set(const struct pico_camera_frame *frames, size_t count,
                           const uint8_t *camera_ids, int64_t max_skew_ns,
                           struct pico_camera_frame_set *set);

#endif
