#ifndef REFRAMED_PICO_CAMERA_TRANSPORT_H
#define REFRAMED_PICO_CAMERA_TRANSPORT_H
#include "pico-camera-request.h"

struct pico_camera_transport;
struct pico_camera_transport_config {
    struct pico_camera_request_config cameras[2];
    int64_t timeout_ns;
};
struct pico_camera_transport_ops {
    bool (*frame)(void *opaque, uint64_t generation,
                  const struct pico_camera_capture_frame *frame, int64_t now_ns);
    void (*invalidate)(void *opaque, uint64_t generation);
};

/* Own a stereo request queue over already configured/acquired/linked devices.
 * Both cameras use the same generation, first request ID, CRM, sync and session;
 * links, camera IDs and device handles are distinct. The caller owns exclusively
 * subscribed nonblocking CRM/sync descriptors for this generation. No device,
 * subscription, sensor, link or thread is created or started here.
 * Calls/callbacks are serialized and non-reentrant; frame must copy its pixels. */
int pico_camera_transport_create(const struct pico_camera_transport_config *config,
                                 const struct pico_camera_transport_ops *ops, void *opaque,
                                 struct pico_camera_transport **transport);
/* Prepare/reserve both images before submitting the next shared request ID.
 * Offsets are calibrated SOF-to-exposure-center values for these requests.
 * At most eight stereo pairs are pending. -EAGAIN means queue capacity only;
 * allocation/submission failures permanently stop this generation. */
int pico_camera_transport_queue(struct pico_camera_transport *transport,
                                const int64_t center_offsets_ns[2], int64_t now_ns);
/* Drain at most 64 events, alternating sources; return consumed count or errno.
 * Call regularly even when descriptors are silent, to enforce timeouts. This
 * does not wait, refill requests, poll the estimator or derive clock alignment. */
int pico_camera_transport_poll(struct pico_camera_transport *transport, int64_t now_ns);
void pico_camera_transport_stop(struct pico_camera_transport *transport);
/* Stop scheduling/callbacks and independently establish device/DMA quiescence
 * before this call. It retires outstanding requests and borrowed reservations.
 * Failed cleanup remains owned; uncertain underlying releases are not retried. */
int pico_camera_transport_quiesced(struct pico_camera_transport *transport);
/* Refuses destruction while requests or reservations remain. */
int pico_camera_transport_destroy(struct pico_camera_transport **transport);
#endif
