#include "pico-camera-transport.h"
#include "pico-camera-stream.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct transport_request {
    struct pico_camera_request *request;
    bool submitted, completed, reserved;
};
struct pico_camera_transport {
    struct pico_camera_transport_config config;
    struct pico_camera_transport_ops ops;
    void *opaque;
    struct pico_camera_stream *stream;
    struct transport_request requests[PICO_CAMERA_CAPTURE_MAX_PENDING];
    uint64_t next_request;
    bool stopped;
};
static struct transport_request *find(struct pico_camera_transport *t, void *token)
{
    for (unsigned i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i)
        if (t->requests[i].request == token) return t->requests + i;
    return NULL;
}
static bool cpu_sync(void *opaque, void *token, bool end)
{
    struct pico_camera_transport *t = opaque;
    struct transport_request *slot = find(t, token);
    if (!slot || !slot->submitted) return false;
    if (!end && !slot->completed) {
        if (pico_camera_request_complete(slot->request)) return false;
        slot->completed = true;
    }
    return !pico_camera_request_cpu_sync(slot->request, end);
}
static bool frame(void *opaque, uint64_t generation,
                  const struct pico_camera_capture_frame *value, int64_t now_ns)
{
    struct pico_camera_transport *t = opaque;
    return t->ops.frame(t->opaque, generation, value, now_ns);
}
static bool release(void *opaque, void *token)
{
    struct pico_camera_transport *t = opaque;
    struct transport_request *slot = find(t, token);
    if (!slot || pico_camera_request_release(&slot->request)) return false;
    memset(slot, 0, sizeof(*slot));
    return true;
}
static void invalidate(void *opaque, uint64_t generation)
{
    struct pico_camera_transport *t = opaque;
    if (!t->stopped) { t->stopped = true; t->ops.invalidate(t->opaque, generation); }
}
void pico_camera_transport_stop(struct pico_camera_transport *t)
{ if (t) pico_camera_stream_stop(t->stream); }
static int fail(struct pico_camera_transport *t, int error)
{ pico_camera_transport_stop(t); return error; }
int pico_camera_transport_create(const struct pico_camera_transport_config *c,
                                 const struct pico_camera_transport_ops *ops, void *opaque,
                                 struct pico_camera_transport **transport)
{
    if (!c || !ops || !transport || *transport || !ops->frame || !ops->invalidate || c->timeout_ns <= 0)
        return -EINVAL;
    const struct pico_camera_request_config *a = c->cameras, *b = a + 1;
    if (!pico_camera_request_config_valid(a) || !pico_camera_request_config_valid(b) ||
        a->crm_fd == a->sync_fd || a->crm_fd != b->crm_fd || a->sync_fd != b->sync_fd ||
        a->session != b->session || a->generation != b->generation || a->request_id != b->request_id ||
        a->link == b->link || a->camera_id == b->camera_id || a->synchronized != b->synchronized ||
        a->sensor == b->sensor || a->sensor == b->ife || a->ife == b->sensor || a->ife == b->ife)
        return -EINVAL;
    struct pico_camera_transport *t = calloc(1, sizeof(*t));
    if (!t) return -ENOMEM;
    t->config = *c; t->ops = *ops; t->opaque = opaque; t->next_request = a->request_id;
    struct pico_camera_stream_config sc = {.capture = {.generation = a->generation,
        .camera_ids = {a->camera_id, b->camera_id}, .camera_count = 2, .timeout_ns = c->timeout_ns},
        .session = a->session, .links = {a->link, b->link}, .first_request_id = a->request_id};
    const struct pico_camera_capture_ops wrapped = {cpu_sync, frame, release, invalidate};
    if (!pico_camera_stream_create(&sc, &wrapped, t, &t->stream)) { free(t); return -ENOMEM; }
    *transport = t;
    return 0;
}
int pico_camera_transport_queue(struct pico_camera_transport *t, const int64_t offsets[2], int64_t now_ns)
{
    if (!t || !offsets || offsets[0] < -1000000000 || offsets[0] > 1000000000 ||
        offsets[1] < -1000000000 || offsets[1] > 1000000000) return -EINVAL;
    if (!pico_camera_stream_poll(t->stream, now_ns)) return fail(t, -EIO);
    if (t->next_request > INT64_MAX) return -EOVERFLOW;
    struct transport_request *slots[2] = {0};
    unsigned found = 0;
    for (unsigned i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING && found < 2; ++i)
        if (!t->requests[i].request) slots[found++] = t->requests + i;
    if (found != 2) return -EAGAIN;
    for (unsigned i = 0; i < 2; ++i) {
        struct pico_camera_request_config c = t->config.cameras[i];
        c.request_id = t->next_request;
        int result = pico_camera_request_prepare(&c, &slots[i]->request);
        if (result) return fail(t, result);
    }
    for (unsigned i = 0; i < 2; ++i) {
        struct pico_camera_capture_buffer buffer;
        if (!pico_camera_request_capture(slots[i]->request, &buffer) ||
            !pico_camera_stream_reserve(t->stream, &buffer, offsets[i], now_ns)) return fail(t, -EINVAL);
        slots[i]->reserved = true;
    }
    ++t->next_request;
    for (unsigned i = 0; i < 2; ++i) {
        slots[i]->submitted = true;
        int result = pico_camera_request_submit(slots[i]->request);
        if (result) return fail(t, result);
    }
    return 0;
}
int pico_camera_transport_poll(struct pico_camera_transport *t, int64_t now_ns)
{
    if (!t) return -EINVAL;
    if (!pico_camera_stream_poll(t->stream, now_ns)) return fail(t, -EIO);
    const int fds[2] = {t->config.cameras[0].sync_fd, t->config.cameras[0].crm_fd};
    bool empty[2] = {false, false};
    int consumed = 0;
    for (unsigned i = 0; i < 64 && !(empty[0] && empty[1]); ++i) {
        unsigned source = i % 2;
        if (empty[source]) continue;
        struct pico_camera_event event;
        int result = pico_camera_event_dequeue(fds[source], &event);
        if (result == -EAGAIN) { empty[source] = true; continue; }
        if (result) return fail(t, result);
        ++consumed;
        if (pico_camera_stream_event(t->stream, (enum pico_camera_event_source)source, &event, now_ns) ==
            PICO_CAMERA_CAPTURE_FAILED) return fail(t, -EIO);
    }
    return consumed;
}
int pico_camera_transport_quiesced(struct pico_camera_transport *t)
{
    if (!t) return -EINVAL;
    pico_camera_transport_stop(t);
    for (unsigned i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) {
        struct transport_request *s = t->requests + i;
        if (!s->request || !s->submitted || s->completed) continue;
        int result = pico_camera_request_complete(s->request);
        if (result) return result;
        s->completed = true;
    }
    if (!pico_camera_stream_quiesced(t->stream)) return -EIO;
    for (unsigned i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) {
        struct transport_request *s = t->requests + i;
        if (!s->request) continue;
        if (s->reserved) return -EBUSY;
        int result = pico_camera_request_release(&s->request);
        if (result) return result;
        memset(s, 0, sizeof(*s));
    }
    return 0;
}
int pico_camera_transport_destroy(struct pico_camera_transport **transport)
{
    if (!transport) return -EINVAL;
    struct pico_camera_transport *t = *transport;
    if (!t) return 0;
    for (unsigned i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i)
        if (t->requests[i].request) return -EBUSY;
    if (!pico_camera_stream_destroy(&t->stream)) return -EBUSY;
    free(t); *transport = NULL;
    return 0;
}
