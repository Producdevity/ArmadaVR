#include "pico-camera-capture.h"
#include <stdlib.h>
#include <string.h>

struct pending_frame {
    struct pico_camera_capture_frame frame;
    int64_t deadline_ns;
    bool used, fence_done, timed;
};

struct pico_camera_capture {
    struct pico_camera_capture_config config;
    struct pico_camera_capture_ops ops;
    void *opaque;
    struct pending_frame pending[PICO_CAMERA_CAPTURE_MAX_PENDING];
    uint64_t last_request[4];
    int64_t now_ns;
    bool stopped;
};

static int camera_index(const struct pico_camera_capture *c, uint8_t id)
{
    for (size_t i = 0; i < c->config.camera_count; ++i)
        if (c->config.camera_ids[i] == id) return (int)i;
    return -1;
}

void pico_camera_capture_stop(struct pico_camera_capture *c)
{
    if (c && !c->stopped) {
        c->stopped = true;
        c->ops.invalidate(c->opaque, c->config.generation);
    }
}

bool pico_camera_capture_create(const struct pico_camera_capture_config *config,
                                 const struct pico_camera_capture_ops *ops, void *opaque,
                                 struct pico_camera_capture **capture)
{
    if (!config || !ops || !capture || *capture || !config->generation || config->timeout_ns <= 0 ||
        (config->camera_count != 2 && config->camera_count != 4) ||
        !ops->cpu_sync || !ops->frame || !ops->release || !ops->invalidate) return false;
    for (size_t i = 0; i < config->camera_count; ++i)
        for (size_t j = 0; j < i; ++j)
            if (config->camera_ids[i] == config->camera_ids[j]) return false;
    struct pico_camera_capture *c = calloc(1, sizeof(*c));
    if (!c) return false;
    c->config = *config;
    c->ops = *ops;
    c->opaque = opaque;
    *capture = c;
    return true;
}

bool pico_camera_capture_poll(struct pico_camera_capture *c, int64_t now_ns)
{
    if (!c || c->stopped) return false;
    if (now_ns <= 0 || now_ns < c->now_ns) {
        pico_camera_capture_stop(c);
        return false;
    }
    c->now_ns = now_ns;
    for (size_t i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) {
        if (c->pending[i].used && now_ns >= c->pending[i].deadline_ns) {
            pico_camera_capture_stop(c);
            return false;
        }
    }
    return true;
}

bool pico_camera_capture_reserve(struct pico_camera_capture *c,
                                  const struct pico_camera_capture_buffer *b, int64_t now_ns)
{
    if (!c || !b || !b->token || !b->request_id || b->request_id > INT64_MAX ||
        b->memory_handle <= 0 || b->fence <= 0 || !b->pixels || !b->width || b->width > 4096 ||
        !b->height || b->height > 4096 || b->stride < b->width || b->stride % 16 ||
        b->stride > SIZE_MAX / b->height || b->size < (size_t)b->stride * b->height ||
        b->size > UINT32_MAX || (uintptr_t)b->pixels > UINTPTR_MAX - b->size) return false;
    int index = camera_index(c, b->camera_id);
    if (index < 0 || b->request_id <= c->last_request[index] ||
        !pico_camera_capture_poll(c, now_ns) || now_ns > INT64_MAX - c->config.timeout_ns) return false;
    struct pending_frame *free_slot = NULL;
    for (size_t i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) {
        struct pending_frame *slot = &c->pending[i];
        if (!slot->used) { if (!free_slot) free_slot = slot; continue; }
        const struct pico_camera_capture_buffer *other = &slot->frame.buffer;
        if (other->token == b->token || other->memory_handle == b->memory_handle || other->fence == b->fence ||
            ((uintptr_t)b->pixels < (uintptr_t)other->pixels + other->size &&
             (uintptr_t)other->pixels < (uintptr_t)b->pixels + b->size)) return false;
    }
    if (!free_slot) return false;
    *free_slot = (struct pending_frame){.used = true, .frame.buffer = *b,
        .deadline_ns = now_ns + c->config.timeout_ns};
    c->last_request[index] = b->request_id;
    return true;
}

static enum pico_camera_capture_result deliver(struct pico_camera_capture *c, struct pending_frame *p)
{
    if (!p->timed || !p->fence_done) return PICO_CAMERA_CAPTURE_PENDING;
    void *token = p->frame.buffer.token;
    if (!c->ops.cpu_sync(c->opaque, token, false)) goto failed;
    bool accepted = c->ops.frame(c->opaque, c->config.generation, &p->frame, c->now_ns);
    bool synchronized = c->ops.cpu_sync(c->opaque, token, true);
    if (!accepted || !synchronized || !c->ops.release(c->opaque, token)) goto failed;
    memset(p, 0, sizeof(*p));
    return PICO_CAMERA_CAPTURE_DELIVERED;
failed:
    pico_camera_capture_stop(c);
    return PICO_CAMERA_CAPTURE_FAILED;
}

enum pico_camera_capture_result pico_camera_capture_fence(
    struct pico_camera_capture *c, uint64_t generation, uint64_t request_id,
    int32_t fence, uint32_t status, int64_t now_ns)
{
    if (!c) return PICO_CAMERA_CAPTURE_FAILED;
    if (generation != c->config.generation) return PICO_CAMERA_CAPTURE_IGNORED;
    if (c->stopped) return PICO_CAMERA_CAPTURE_FAILED;
    for (size_t i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) {
        struct pending_frame *p = &c->pending[i];
        if (!p->used || p->frame.buffer.request_id != request_id || p->frame.buffer.fence != fence) continue;
        if (!pico_camera_capture_poll(c, now_ns)) return PICO_CAMERA_CAPTURE_FAILED;
        if (status != 2) { pico_camera_capture_stop(c); return PICO_CAMERA_CAPTURE_FAILED; }
        p->fence_done = true;
        return deliver(c, p);
    }
    return PICO_CAMERA_CAPTURE_IGNORED;
}

enum pico_camera_capture_result pico_camera_capture_timing(
    struct pico_camera_capture *c, uint64_t generation, uint64_t request_id,
    uint8_t camera_id, uint64_t frame_number, int64_t center_ns, int64_t now_ns)
{
    if (!c) return PICO_CAMERA_CAPTURE_FAILED;
    if (generation != c->config.generation) return PICO_CAMERA_CAPTURE_IGNORED;
    if (c->stopped) return PICO_CAMERA_CAPTURE_FAILED;
    for (size_t i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) {
        struct pending_frame *p = &c->pending[i];
        if (!p->used || p->frame.buffer.request_id != request_id || p->frame.buffer.camera_id != camera_id) continue;
        if (!pico_camera_capture_poll(c, now_ns)) return PICO_CAMERA_CAPTURE_FAILED;
        if (center_ns <= 0 || (p->timed && (p->frame.center_ns != center_ns || p->frame.frame_number != frame_number))) {
            pico_camera_capture_stop(c);
            return PICO_CAMERA_CAPTURE_FAILED;
        }
        p->frame.center_ns = center_ns;
        p->frame.frame_number = frame_number;
        p->timed = true;
        return deliver(c, p);
    }
    return PICO_CAMERA_CAPTURE_IGNORED;
}

bool pico_camera_capture_quiesced(struct pico_camera_capture *c)
{
    if (!c || !c->stopped) return false;
    bool released = true;
    for (size_t i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) {
        struct pending_frame *p = &c->pending[i];
        if (!p->used) continue;
        if (c->ops.release(c->opaque, p->frame.buffer.token)) memset(p, 0, sizeof(*p));
        else released = false;
    }
    return released;
}

bool pico_camera_capture_destroy(struct pico_camera_capture **capture)
{
    if (!capture) return false;
    if (!*capture) return true;
    struct pico_camera_capture *c = *capture;
    for (size_t i = 0; i < PICO_CAMERA_CAPTURE_MAX_PENDING; ++i) if (c->pending[i].used) return false;
    pico_camera_capture_stop(c);
    free(c);
    *capture = NULL;
    return true;
}
