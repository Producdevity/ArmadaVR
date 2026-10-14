#ifndef REFRAMED_PICO_CAMERA_REQUEST_H
#define REFRAMED_PICO_CAMERA_REQUEST_H

#include "pico-camera-buffer.h"
#include "pico-camera-capture.h"

struct pico_camera_request;
struct pico_camera_request_config {
    int crm_fd, sync_fd, ion_fd, sensor_fd, ife_fd;
    int32_t session, link, sensor, ife, device_mmu, command_mmu;
    enum pico_camera_memory_mapping command_mapping;
    uint32_t ion_heap, width, height, stride;
    uint8_t camera_id;
    uint64_t generation, request_id;
    bool synchronized;
};

bool pico_camera_request_config_valid(const struct pico_camera_request_config *config);

/* Prepare one mono8 RDI request for already initialized, linked devices. The
 * caller supplies queried MMUs, the selected ION heap and calibrated geometry.
 * The request owns pixel/command/packet buffers and a registered fence; it opens
 * no device and does not start a sensor. Even failed preparation may return a
 * nonnull owner which must be released. Calls are serialized/non-reentrant. */
int pico_camera_request_prepare(const struct pico_camera_request_config *config,
                                struct pico_camera_request **request);
bool pico_camera_request_capture(const struct pico_camera_request *request,
                                 struct pico_camera_capture_buffer *buffer);
/* Reserve the capture buffer in the matching stream before calling submit.
 * Schedule once, then configure sensor NOP and IFE output. An error ends the
 * generation; stop the stream and retain every resource until verified quiescence.
 * No operation is retried and no flush is inferred to stop DMA. */
int pico_camera_request_submit(struct pico_camera_request *request);
/* Call only after this request's successful fence, or independent confirmation
 * that all capture DMA has stopped. Enables CPU access/release, not resubmission. */
int pico_camera_request_complete(struct pico_camera_request *request);
int pico_camera_request_cpu_sync(struct pico_camera_request *request, bool end);
/* Release after the stream has finished its borrowed frame. Refuses in-flight
 * resources. Uncertain fence destruction is retained without a blind retry. */
int pico_camera_request_release(struct pico_camera_request **request);

#endif
