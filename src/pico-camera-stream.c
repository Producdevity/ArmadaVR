#include "pico-camera-stream.h"
#include "pico-camera-events.h"
#include <stdlib.h>
#include <string.h>

struct stream_request {
    void *token;
    uint64_t id;
    uint8_t camera;
    int64_t center_offset_ns;
};
struct pico_camera_stream {
    struct pico_camera_stream_config config;
    struct pico_camera_capture_ops ops;
    void *opaque;
    struct pico_camera_capture *capture;
    struct stream_request requests[PICO_CAMERA_CAPTURE_MAX_PENDING];
    uint32_t sequence[2];
    bool have_sequence[2], stopped;
};

static bool cpu_sync(void *opaque, void *token, bool end)
{
    struct pico_camera_stream *s = opaque;
    return s->ops.cpu_sync(s->opaque, token, end);
}
static bool frame(void *opaque, uint64_t generation,
                  const struct pico_camera_capture_frame *value, int64_t now)
{
    struct pico_camera_stream *s = opaque;
    return s->ops.frame(s->opaque, generation, value, now);
}
static bool release(void *opaque, void *token)
{
    struct pico_camera_stream *s = opaque;
    for (unsigned i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) {
        if (s->requests[i].token != token) continue;
        if (!s->ops.release(s->opaque, token)) return false;
        memset(&s->requests[i], 0, sizeof(s->requests[i]));
        return true;
    }
    return false;
}
static void invalidate(void *opaque, uint64_t generation)
{
    struct pico_camera_stream *s = opaque;
    s->stopped = true;
    s->ops.invalidate(s->opaque, generation);
}
bool pico_camera_stream_create(const struct pico_camera_stream_config *config,
                                const struct pico_camera_capture_ops *ops, void *opaque,
                                struct pico_camera_stream **stream)
{
    if (!config || !ops || !stream || *stream || !ops->cpu_sync || !ops->frame ||
        !ops->release || !ops->invalidate || config->session <= 0 || !config->first_request_id ||
        config->first_request_id > INT64_MAX ||
        (config->capture.camera_count != 2 && config->capture.camera_count != 4)) return false;
    for (size_t i = 0; i < config->capture.camera_count; ++i) {
        if (config->links[i] <= 0) return false;
        for (size_t j = 0; j < i; ++j) if (config->links[i] == config->links[j]) return false;
    }
    struct pico_camera_stream *s = calloc(1, sizeof(*s));
    if (!s) return false;
    s->config = *config; s->ops = *ops; s->opaque = opaque;
    const struct pico_camera_capture_ops wrapped = {cpu_sync, frame, release, invalidate};
    if (!pico_camera_capture_create(&config->capture, &wrapped, s, &s->capture)) { free(s); return false; }
    *stream = s;
    return true;
}
bool pico_camera_stream_reserve(struct pico_camera_stream *s,
                                 const struct pico_camera_capture_buffer *buffer,
                                 int64_t center_offset_ns, int64_t now_ns)
{
    if (!s || !buffer || s->stopped || buffer->request_id < s->config.first_request_id ||
        center_offset_ns < -1000000000 || center_offset_ns > 1000000000) return false;
    for (unsigned i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) {
        if (s->requests[i].token) continue;
        if (!pico_camera_capture_reserve(s->capture, buffer, now_ns)) return false;
        s->requests[i] = (struct stream_request){buffer->token, buffer->request_id,
                                                buffer->camera_id, center_offset_ns};
        return true;
    }
    return false;
}
bool pico_camera_stream_poll(struct pico_camera_stream *s, int64_t now_ns)
{
    return s && pico_camera_capture_poll(s->capture, now_ns);
}
void pico_camera_stream_stop(struct pico_camera_stream *s)
{
    if (s) pico_camera_capture_stop(s->capture);
}
static enum pico_camera_capture_result fail(struct pico_camera_stream *s)
{
    pico_camera_stream_stop(s);
    return PICO_CAMERA_CAPTURE_FAILED;
}
static int link_index(const struct pico_camera_stream *s, int32_t session, int32_t link)
{
    if (session != s->config.session) return -1;
    for (size_t i = 0; i < s->config.capture.camera_count; ++i)
        if (s->config.links[i] == link) return (int)i;
    return -1;
}
enum pico_camera_capture_result pico_camera_stream_event(
    struct pico_camera_stream *s, enum pico_camera_event_source source,
    const struct pico_camera_event *event, int64_t now_ns)
{
    if (!s) return PICO_CAMERA_CAPTURE_FAILED;
    if (!event || source < PICO_CAMERA_SYNC_EVENTS || source > PICO_CAMERA_CRM_EVENTS ||
        !pico_camera_stream_poll(s, now_ns)) return fail(s);
    unsigned channel = (unsigned)source;
    if (s->have_sequence[channel] && event->sequence != s->sequence[channel] + UINT32_C(1)) return fail(s);
    s->have_sequence[channel] = true; s->sequence[channel] = event->sequence;
    if (source == PICO_CAMERA_SYNC_EVENTS) {
        struct pico_camera_fence_event fence;
        enum pico_camera_event_result result = pico_camera_fence_event_decode(
            event->type, event->id, event->data, sizeof(event->data), &fence);
        if (result == PICO_CAMERA_EVENT_MALFORMED) return fail(s);
        if (result == PICO_CAMERA_EVENT_UNRELATED) return PICO_CAMERA_CAPTURE_IGNORED;
        return pico_camera_capture_fence(s->capture, fence.generation, fence.request_id,
                                         fence.fence, fence.status, now_ns);
    }
    struct pico_camera_error_event error;
    enum pico_camera_event_result result = pico_camera_error_event_decode(
        event->type, event->id, event->data, sizeof(event->data), &error);
    if (result == PICO_CAMERA_EVENT_MALFORMED) return fail(s);
    if (result == PICO_CAMERA_EVENT_VALID)
        return link_index(s, error.session, error.link) < 0 ? PICO_CAMERA_CAPTURE_IGNORED : fail(s);
    struct pico_camera_sof_event sof;
    result = pico_camera_sof_event_decode(event->type, event->id, event->data, sizeof(event->data), &sof);
    if (result == PICO_CAMERA_EVENT_MALFORMED) return fail(s);
    if (result == PICO_CAMERA_EVENT_UNRELATED) return PICO_CAMERA_CAPTURE_IGNORED;
    int index = link_index(s, sof.session, sof.link);
    if (index < 0) return PICO_CAMERA_CAPTURE_IGNORED;
    if (sof.status) return fail(s);
    if (sof.request_id < s->config.first_request_id) return PICO_CAMERA_CAPTURE_IGNORED;
    uint8_t camera = s->config.capture.camera_ids[index];
    for (unsigned i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) {
        const struct stream_request *request = &s->requests[i];
        if (!request->token || request->id != sof.request_id || request->camera != camera) continue;
        int64_t offset = request->center_offset_ns;
        if ((offset > 0 && sof.qtimer_ns > INT64_MAX - offset) ||
            (offset < 0 && sof.qtimer_ns <= -offset)) return fail(s);
        return pico_camera_capture_timing(s->capture, s->config.capture.generation,
            sof.request_id, camera, sof.frame_number, sof.qtimer_ns + offset, now_ns);
    }
    return PICO_CAMERA_CAPTURE_IGNORED;
}
bool pico_camera_stream_quiesced(struct pico_camera_stream *s)
{
    return s && pico_camera_capture_quiesced(s->capture);
}
bool pico_camera_stream_destroy(struct pico_camera_stream **stream)
{
    if (!stream) return false;
    if (!*stream) return true;
    if (!pico_camera_capture_destroy(&(*stream)->capture)) return false;
    free(*stream); *stream = NULL;
    return true;
}
