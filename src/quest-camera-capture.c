#include "quest-camera-capture.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct quest_camera_capture {
    struct quest_camera_capture_config config;
    struct quest_camera_capture_ops ops;
    void *opaque;
    struct quest_camera_shutter_candidate shutters[16];
    size_t shutter_count;
    struct quest_camera_capture_frame pending;
    uint8_t *mono;
    uint64_t minimum_us, maximum_us, consumed_us;
    int64_t now_ns, delivered_ns, center_ns, number, qtimer;
    bool started, stopped, quiesced, held, have_image;
};

void quest_camera_capture_stop(struct quest_camera_capture *c)
{
    if (c && !c->stopped) {
        c->stopped = true;
        if (c->started) c->ops.invalidate(c->opaque, c->config.generation);
    }
}
static int fail(struct quest_camera_capture *c, int error)
{
    quest_camera_capture_stop(c);
    return error;
}
static bool clock_valid(struct quest_camera_capture *c, int64_t now)
{
    if (!c || !c->started || c->stopped) return false;
    if (now <= 0 || now < c->now_ns || now - c->delivered_ns >= c->config.timeout_ns) {
        quest_camera_capture_stop(c);
        return false;
    }
    c->now_ns = now;
    return true;
}
static void prune(struct quest_camera_capture *c)
{
    size_t count = 0;
    for (size_t i = 0; i < c->shutter_count; ++i) {
        const struct quest_camera_shutter_candidate *s = &c->shutters[i];
        if (s->shutter.timestamp_us > c->consumed_us &&
            c->now_ns - s->received_ns <= c->config.timeout_ns) c->shutters[count++] = *s;
    }
    c->shutter_count = count;
}

bool quest_camera_capture_create(const struct quest_camera_capture_config *config,
                                  const struct quest_camera_capture_ops *ops, void *opaque,
                                  struct quest_camera_capture **capture)
{
    if (!config || !ops || !capture || *capture || config->fd < 0 || config->session <= 0 ||
        config->device <= 0 || !config->generation || !config->camera_bit ||
        (config->camera_bit & (config->camera_bit - 1u)) || !config->buffer_count ||
        config->buffer_count > 30 || config->timeout_ns <= 0 || config->timeout_ns > 1000000000 ||
        !ops->cpu_sync || !ops->window || !ops->center || !ops->frame || !ops->invalidate ||
        (config->sensor != QUEST_CAMERA_OG01A1B && config->sensor != QUEST_CAMERA_OV7251) ||
        (config->metadata_bits != 8 && config->metadata_bits != 10) ||
        !config->metadata.mcu_words[0] || !config->metadata_size || config->metadata_size > 16384)
        return false;
    const struct quest_camera_pixel_layout *l = &config->layout;
    if (!l->width || l->width > 4096 || !l->height || l->height > 4096 || l->first_row != 1 ||
        !l->stride || l->stride > 65536 || config->metadata_size > l->stride ||
        (l->format != QUEST_CAMERA_MONO8 && l->format != QUEST_CAMERA_MIPI_RAW10) ||
        (l->curve != QUEST_CAMERA_CURVE_COPY && l->curve != QUEST_CAMERA_CURVE_POINT5 &&
         l->curve != QUEST_CAMERA_CURVE_POINT5_OFFSET) ||
        (l->format == QUEST_CAMERA_MONO8 && l->curve != QUEST_CAMERA_CURVE_COPY) ||
        (l->format == QUEST_CAMERA_MIPI_RAW10 && l->width % 4)) return false;
    size_t row = l->format == QUEST_CAMERA_MONO8 ? l->width : l->width / 4u * 5u;
    if (l->stride < row) return false;
    for (size_t i = 0; i < config->buffer_count; ++i) {
        const struct quest_camera_capture_buffer *b = &config->buffers[i];
        if (!b->id || !b->token || !b->pixels || b->size < l->stride * l->height + row ||
            b->size > 65536u * 4097u || (uintptr_t)b->pixels > UINTPTR_MAX - b->size) return false;
        for (size_t j = 0; j < i; ++j) {
            const struct quest_camera_capture_buffer *a = &config->buffers[j];
            if (a->id == b->id || a->token == b->token ||
                ((uintptr_t)a->pixels < (uintptr_t)b->pixels + b->size &&
                 (uintptr_t)b->pixels < (uintptr_t)a->pixels + a->size)) return false;
        }
    }
    struct quest_camera_capture *c = calloc(1, sizeof(*c));
    if (!c) return false;
    c->mono = malloc((size_t)l->width * l->height);
    if (!c->mono) { free(c); return false; }
    c->config = *config; c->ops = *ops; c->opaque = opaque;
    *capture = c;
    return true;
}
bool quest_camera_capture_start(struct quest_camera_capture *c, int64_t now)
{
    if (!c || c->started || c->stopped || now <= 0) return false;
    c->started = true;
    c->now_ns = c->delivered_ns = now;
    return true;
}
bool quest_camera_capture_shutter(struct quest_camera_capture *c, uint64_t generation,
                                   const struct quest_camera_shutter *s, int64_t now)
{
    if (!c) return false;
    if (generation != c->config.generation) return true;
    if (!clock_valid(c, now)) return false;
    if (!s || !s->timestamp_us || !(s->camera_mask & c->config.camera_bit) ||
        (s->type != 6 && s->type != 21 && s->type != 22)) return fail(c, -EPROTO), false;
    if (s->timestamp_us <= c->consumed_us) return true;
    prune(c);
    for (size_t i = 0; i < c->shutter_count; ++i)
        if (c->shutters[i].shutter.timestamp_us == s->timestamp_us) return fail(c, -EEXIST), false;
    if (c->shutter_count == 16) return fail(c, -ENOSPC), false;
    c->shutters[c->shutter_count++] = (struct quest_camera_shutter_candidate){*s, generation, now};
    return true;
}
int quest_camera_capture_poll(struct quest_camera_capture *c, int64_t now)
{
    if (!c || !c->started) return -EINVAL;
    if (!clock_valid(c, now)) return -EPIPE;
    const struct quest_camera_capture_config *cfg = &c->config;
    if (!c->held) {
        struct quest_camera_frame image;
        int rc = quest_camera_buffer_get(cfg->fd, cfg->session, cfg->device, &image);
        if (rc == -ETIMEDOUT || rc == -ENOENT) return 0;
        if (rc) return fail(c, rc);
        c->held = true;
        const struct quest_camera_capture_buffer *b = NULL;
        for (size_t i = 0; i < cfg->buffer_count; ++i)
            if (cfg->buffers[i].id == image.id) b = &cfg->buffers[i];
        if (!b || image.observed_monotonic_ns <= 0 || image.observed_monotonic_ns > now ||
            now - image.observed_monotonic_ns >= cfg->timeout_ns ||
            (c->have_image && (image.frame_number <= c->number || image.qtimer_ns <= c->qtimer)))
            return fail(c, -EPROTO);
        c->pending = (struct quest_camera_capture_frame){.pixels = c->mono,
            .width = cfg->layout.width, .height = cfg->layout.height, .image = image};
        if (!c->ops.cpu_sync(c->opaque, b->token, false)) return fail(c, -EIO);
        bool decoded = quest_camera_metadata_decode(b->pixels, cfg->metadata_size, cfg->sensor,
                                                     &cfg->metadata, &c->pending.metadata) &&
                       c->pending.metadata.bits_per_pixel == cfg->metadata_bits &&
                       quest_camera_pixels_mono8(b->pixels, b->size, &cfg->layout, c->mono,
                                                  (size_t)cfg->layout.width * cfg->layout.height);
        bool synchronized = c->ops.cpu_sync(c->opaque, b->token, true);
        if (!decoded || !synchronized) return fail(c, -EIO);
        c->minimum_us = c->maximum_us = 0;
        if (!c->ops.window(c->opaque, &image, &c->pending.metadata, now, &c->minimum_us, &c->maximum_us) ||
            !c->minimum_us || c->maximum_us < c->minimum_us || c->maximum_us <= c->consumed_us)
            return fail(c, -ERANGE);
        c->number = image.frame_number; c->qtimer = image.qtimer_ns; c->have_image = true;
    }
    if (now - c->pending.image.observed_monotonic_ns >= cfg->timeout_ns)
        return fail(c, -ETIMEDOUT);
    prune(c);
    struct quest_camera_match_window window = {cfg->generation, c->consumed_us,
        c->minimum_us, c->maximum_us, now, cfg->timeout_ns, cfg->camera_bit, cfg->require_auxiliary_tag};
    int rc = quest_camera_shutter_select(&c->pending.metadata, &window, c->shutters,
                                          c->shutter_count, &c->pending.shutter);
    if (rc < 0) return fail(c, rc);
    if (!rc) return 0;
    c->pending.center_ns = 0;
    if (!c->ops.center(c->opaque, &c->pending.image, &c->pending.metadata, &c->pending.shutter,
                       now, &c->pending.center_ns) || c->pending.center_ns <= c->center_ns)
        return fail(c, -ERANGE);
    if (!c->ops.frame(c->opaque, cfg->generation, &c->pending, now)) return fail(c, -EIO);
    rc = quest_camera_buffer_return(cfg->fd, cfg->session, cfg->device, c->pending.image.id);
    if (rc) return fail(c, rc);
    c->held = false;
    c->consumed_us = c->pending.shutter.timestamp_us;
    c->center_ns = c->pending.center_ns;
    c->delivered_ns = now;
    return 1;
}
bool quest_camera_capture_quiesced(struct quest_camera_capture *c)
{
    if (!c || !c->stopped) return false;
    c->quiesced = true;
    return true;
}
bool quest_camera_capture_destroy(struct quest_camera_capture **capture)
{
    if (!capture) return false;
    struct quest_camera_capture *c = *capture;
    if (!c) return true;
    if (c->started && !c->quiesced) return false;
    quest_camera_capture_stop(c);
    free(c->mono);
    free(c);
    *capture = NULL;
    return true;
}
