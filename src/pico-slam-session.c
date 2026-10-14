#include "pico-slam-session.h"
#include <xrt/xrt_frame.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct camera_sink {
    struct xrt_frame_sink base;
    struct pico_slam_session *owner;
    uint32_t index;
};
struct imu_sink {
    struct xrt_imu_sink base;
    struct pico_slam_session *owner;
};
struct pico_slam_session {
    struct pico_slam_backend api;
    struct pico_slam_input_config config;
    struct pico_slam_input *input;
    struct pico_slam_tracking *tracking;
    struct camera_sink cameras[4];
    struct imu_sink imu;
    vit_tracker_t *tracker;
    char *backend_config;
    uint64_t generation;
    int64_t started, now, expires, uncertainty, last_camera, last_imu;
    bool active, failed;
};

static void invalidate(struct pico_slam_session *s)
{
    s->active = false;
    pico_slam_tracking_stop(s->tracking);
    pico_slam_input_stop(s->input);
}

static void camera(struct xrt_frame_sink *sink, struct xrt_frame *f)
{
    struct camera_sink *c = (struct camera_sink *)sink;
    struct pico_slam_session *s = c->owner;
    if (s->failed) return;
    vit_img_sample_t image = {.cam_index = c->index, .timestamp = f->timestamp,
        .data = f->data, .width = f->width, .height = f->height,
        .stride = (uint32_t)f->stride, .size = (uint32_t)f->size, .format = VIT_IMAGE_FORMAT_L8};
    if (s->api.image(s->tracker, &image) != VIT_SUCCESS) s->failed = true;
    else if (c->index + 1 == s->config.count) s->last_camera = f->timestamp;
}

static void imu(struct xrt_imu_sink *sink, struct xrt_imu_sample *sample)
{
    struct pico_slam_session *s = ((struct imu_sink *)sink)->owner;
    if (s->failed) return;
    vit_imu_sample_t value = {.timestamp = sample->timestamp_ns,
        .ax = sample->accel_m_s2.x, .ay = sample->accel_m_s2.y, .az = sample->accel_m_s2.z,
        .wx = sample->gyro_rad_secs.x, .wy = sample->gyro_rad_secs.y, .wz = sample->gyro_rad_secs.z};
    if (!isfinite(value.ax) || !isfinite(value.ay) || !isfinite(value.az) ||
        !isfinite(value.wx) || !isfinite(value.wy) || !isfinite(value.wz) ||
        s->api.imu(s->tracker, &value) != VIT_SUCCESS) s->failed = true;
    else s->last_imu = sample->timestamp_ns;
}

bool pico_slam_session_create(const struct pico_slam_input_config *input,
                              const struct pico_slam_tracking_config *tracking,
                              const struct pico_slam_backend *api, const char *file,
                              struct pico_slam_session **session)
{
    if (!input || !tracking || !api || !file || !*file || !session || *session ||
        !api->version || !api->create || !api->destroy || !api->has_format || !api->start ||
        !api->stop || !api->imu || !api->image || !api->pop || !api->pose_data || !api->pose_destroy ||
        (input->count != 2 && input->count != 4)) return false;
    uint32_t major, minor, patch;
    if (api->version(&major, &minor, &patch) != VIT_SUCCESS || major != 2 || minor != 0 || patch != 1)
        return false;
    struct pico_slam_session *s = calloc(1, sizeof(*s));
    if (!s) return false;
    s->api = *api; s->config = *input;
    s->backend_config = malloc(strlen(file) + 1);
    if (!s->backend_config) { free(s); return false; }
    strcpy(s->backend_config, file);
    struct xrt_slam_sinks sinks = {.cam_count = (int)input->count, .imu = &s->imu.base};
    s->imu = (struct imu_sink){{imu}, s};
    for (uint32_t i = 0; i < input->count; ++i) {
        s->cameras[i] = (struct camera_sink){{camera}, s, i};
        sinks.cams[i] = &s->cameras[i].base;
    }
    if (!pico_slam_tracking_create(tracking, &s->tracking) ||
        !pico_slam_input_create(input, &sinks, &s->input)) {
        pico_slam_session_destroy(&s); return false;
    }
    *session = s;
    return true;
}

void pico_slam_session_stop(struct pico_slam_session *s)
{
    if (!s) return;
    invalidate(s);
    if (s->tracker) {
        s->api.stop(s->tracker);
        s->api.destroy(s->tracker);
        s->tracker = NULL;
    }
}

bool pico_slam_session_begin(struct pico_slam_session *s, uint64_t generation,
                             int64_t offset, int64_t uncertainty, int64_t expires, int64_t now)
{
    if (!s || generation == 0 || generation <= s->generation || now <= 0 || expires <= now ||
        uncertainty < 0 || uncertainty > s->config.max_skew_ns / 2) return false;
    pico_slam_session_stop(s);
    s->generation = generation; s->started = s->now = now; s->expires = expires;
    s->uncertainty = uncertainty; s->last_camera = s->last_imu = 0; s->failed = false;
    vit_config_t config = {.file = s->backend_config, .cam_count = (uint32_t)s->config.count, .imu_count = 1};
    bool format = false;
    if (s->api.create(&config, &s->tracker) != VIT_SUCCESS || !s->tracker ||
        s->api.has_format(s->tracker, VIT_IMAGE_FORMAT_L8, &format) != VIT_SUCCESS || !format ||
        s->api.start(s->tracker) != VIT_SUCCESS ||
        !pico_slam_input_begin(s->input, generation, offset, uncertainty, expires) ||
        !pico_slam_tracking_begin(s->tracking, generation)) {
        pico_slam_session_stop(s); return false;
    }
    s->active = true;
    return true;
}

static bool current(struct pico_slam_session *s, uint64_t generation, int64_t now)
{
    if (!s || !s->active || generation != s->generation) return false;
    if (now < s->now || now > s->expires ||
        now - (s->last_camera ? s->last_camera : s->started) > s->config.max_gap_ns ||
        now - (s->last_imu ? s->last_imu : s->started) > s->config.max_gap_ns) {
        invalidate(s); return false;
    }
    s->now = now;
    return true;
}

bool pico_slam_session_camera(struct pico_slam_session *s, uint64_t generation,
                              const void *snapshot, size_t size, int64_t now)
{
    if (!current(s, generation, now)) return false;
    if (!pico_slam_input_camera(s->input, generation, snapshot, size, now) || s->failed) {
        invalidate(s); return false;
    }
    return true;
}

bool pico_slam_session_mono8(struct pico_slam_session *s, uint64_t generation,
                             const struct pico_slam_mono8_frame *frame, int64_t now)
{
    if (!current(s, generation, now)) return false;
    if (!pico_slam_input_mono8(s->input, generation, frame, now) || s->failed) {
        invalidate(s); return false;
    }
    return true;
}

bool pico_slam_session_captured(struct pico_slam_session *s, uint64_t generation,
                                const struct pico_camera_capture_frame *f, int64_t now)
{
    if (!f) return pico_slam_session_mono8(s, generation, NULL, now);
    const struct pico_camera_capture_buffer *b = &f->buffer;
    struct pico_slam_mono8_frame frame = {b->camera_id, b->width, b->height,
        b->stride, b->size, b->pixels, f->center_ns, f->frame_number};
    return pico_slam_session_mono8(s, generation, &frame, now);
}

bool pico_slam_session_imu(struct pico_slam_session *s, uint64_t generation,
                           const struct xrt_imu_sample *sample, int64_t uncertainty, int64_t now)
{
    if (!current(s, generation, now)) return false;
    if (!pico_slam_input_imu(s->input, generation, sample, uncertainty, now) || s->failed) {
        invalidate(s); return false;
    }
    if (uncertainty > s->uncertainty) s->uncertainty = uncertainty;
    return true;
}

bool pico_slam_session_poll(struct pico_slam_session *s, uint64_t generation, int64_t now, size_t *poses)
{
    if (!poses) return false;
    *poses = 0;
    if (!current(s, generation, now)) return false;
    for (size_t i = 0; i < 32; ++i) {
        vit_pose_t *pose = NULL;
        if (s->api.pop(s->tracker, &pose) != VIT_SUCCESS) {
            if (pose) s->api.pose_destroy(pose);
            goto fail;
        }
        if (!pose) return true;
        vit_pose_data_t data;
        bool ok = s->api.pose_data(pose, &data) == VIT_SUCCESS;
        s->api.pose_destroy(pose);
        if (!ok) goto fail;
        struct xrt_pose estimate = {{data.ox, data.oy, data.oz, data.ow}, {data.px, data.py, data.pz}};
        if (!pico_slam_tracking_push(s->tracking, generation, &estimate, data.timestamp, s->uncertainty, now))
            goto fail;
        ++*poses;
    }
    return true;
fail:
    *poses = 0;
    invalidate(s);
    return false;
}

struct pico_slam_tracking *pico_slam_session_tracking(struct pico_slam_session *s)
{
    return s ? s->tracking : NULL;
}

void pico_slam_session_destroy(struct pico_slam_session **session)
{
    if (!session || !*session) return;
    struct pico_slam_session *s = *session;
    *session = NULL;
    pico_slam_session_stop(s);
    pico_slam_input_destroy(&s->input);
    pico_slam_tracking_destroy(&s->tracking);
    free(s->backend_config);
    free(s);
}
