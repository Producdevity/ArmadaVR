#ifndef REFRAMED_PICO_CAMERA_STREAM_H
#define REFRAMED_PICO_CAMERA_STREAM_H

#include "pico-camera-capture.h"
#include "pico-camera-control.h"

struct pico_camera_stream;
struct pico_camera_stream_config {
    struct pico_camera_capture_config capture;
    int32_t session, links[4];
    uint64_t first_request_id;
};
enum pico_camera_event_source { PICO_CAMERA_SYNC_EVENTS, PICO_CAMERA_CRM_EVENTS };

/* Owns a capture generation and routes events from its exclusive sync/CRM fds.
 * Links correspond to capture.camera_ids. Use newly acquired links and never
 * reuse request IDs for the same session/link across generations. The request
 * floor rejects earlier SOFs, whose payload has no generation cookie.
 * Calls and callbacks are serialized; no fd is opened and no ioctl is issued. */
bool pico_camera_stream_create(const struct pico_camera_stream_config *config,
                                const struct pico_camera_capture_ops *ops, void *opaque,
                                struct pico_camera_stream **stream);

/* Reserve before submission, with the calibrated SOF-to-exposure-center offset
 * for this request. An offset must account for the selected sensor mode and
 * exposure, not its request pipeline delay. No default offset is supplied. */
bool pico_camera_stream_reserve(struct pico_camera_stream *stream,
                                 const struct pico_camera_capture_buffer *buffer,
                                 int64_t sof_to_center_ns, int64_t now_ns);

enum pico_camera_capture_result pico_camera_stream_event(
    struct pico_camera_stream *stream, enum pico_camera_event_source source,
    const struct pico_camera_event *event, int64_t now_ns);
bool pico_camera_stream_poll(struct pico_camera_stream *stream, int64_t now_ns);
void pico_camera_stream_stop(struct pico_camera_stream *stream);
/* Only after independent confirmation that all capture DMA has stopped. */
bool pico_camera_stream_quiesced(struct pico_camera_stream *stream);
bool pico_camera_stream_destroy(struct pico_camera_stream **stream);

#endif
