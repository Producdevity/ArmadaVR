#include "pico-slam-live.h"
#include "pico-camera-transport.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

struct pico_slam_live {
    struct pico_slam_runtime *runtime;
    struct pico_camera_rig *rig;
    struct pico_camera_transport *transport;
    struct { uint64_t request; unsigned complete; } pending[8];
    uint64_t generation, next_request;
    uint8_t camera_ids[2];
    unsigned queue_depth;
    int event_fds[3];
    bool subscriptions[3], ready, streaming, failed, stopped;
    bool quiescence_attempted, quiesced, release_attempted;
    int stop_error, cleanup_error;
};

static void invalidate(void *opaque, uint64_t generation)
{
    struct pico_slam_live *s = opaque;
    (void)generation;
    s->failed = true;
    pico_slam_runtime_stop(s->runtime);
}

static bool frame(void *opaque, uint64_t generation,
                  const struct pico_camera_capture_frame *value, int64_t now)
{
    struct pico_slam_live *s = opaque;
    if (s->failed || s->stopped || !value || generation != s->generation) return false;
    unsigned camera = value->buffer.camera_id == s->camera_ids[0] ? 0 : 1;
    if (value->buffer.camera_id != s->camera_ids[camera]) return false;
    for (unsigned i = 0; i < s->queue_depth; ++i) {
        if (!s->pending[i].request || s->pending[i].request != value->buffer.request_id) continue;
        unsigned bit = 1U << camera;
        if (s->pending[i].complete & bit ||
            !pico_slam_runtime_captured(s->runtime, generation, value, now)) return false;
        s->pending[i].complete |= bit;
        if (s->pending[i].complete == 3) {
            s->pending[i].request = 0;
            s->pending[i].complete = 0;
        }
        return true;
    }
    return false;
}

static bool valid(const struct pico_slam_live_config *c)
{
    if (!c || c->sync_fd < 0 || c->ion_fd < 0 || c->sync_fd == c->ion_fd ||
        c->sync_fd == c->cameras.session.crm_fd || c->ion_fd == c->cameras.session.crm_fd ||
        c->tracking.input.count != 2 || c->tracking.input.camera_ids[0] == c->tracking.input.camera_ids[1] ||
        !c->queue_depth || c->queue_depth > 8 || !c->first_request_id || c->first_request_id > INT64_MAX ||
        c->frame_period_ns <= 0 || c->frame_period_ns > c->tracking.input.max_gap_ns ||
        c->capture_timeout_ns <= 0 || c->frame_period_ns > c->capture_timeout_ns / (c->queue_depth + 1)) return false;
    for (unsigned i = 0; i < 2; ++i) {
        const struct pico_camera_ife_config *input = &c->cameras.session.cameras[i].input;
        if (input->width != c->tracking.input.width[i] || input->height != c->tracking.input.height[i] ||
            c->stride[i] < input->width || c->stride[i] % 16) return false;
    }
    return true;
}

int pico_slam_live_prepare(const struct pico_slam_live_config *c, struct pico_slam_live **live)
{
    if (!live || *live || !valid(c)) return -EINVAL;
    int fds[] = {c->cameras.session.crm_fd, c->sync_fd};
    for (unsigned i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL);
        if (flags < 0) return -errno;
        if (!(flags & O_NONBLOCK)) return -EINVAL;
    }
    struct pico_slam_live *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    *live = s;
    s->generation = c->tracking.generation;
    s->next_request = c->first_request_id;
    s->queue_depth = c->queue_depth;
    s->event_fds[0] = s->event_fds[1] = c->cameras.session.crm_fd;
    s->event_fds[2] = c->sync_fd;
    for (unsigned i = 0; i < 2; ++i) s->camera_ids[i] = c->tracking.input.camera_ids[i];
    if (!pico_slam_runtime_prepare(&c->tracking, &s->runtime)) return -EINVAL;
    int result = pico_camera_rig_prepare(&c->cameras, &s->rig);
    if (result) return result;
    struct pico_camera_session_view view;
    if (!pico_camera_rig_view(s->rig, &view)) return -EPROTO;
    struct pico_camera_transport_config transport = {.timeout_ns = c->capture_timeout_ns};
    for (unsigned i = 0; i < 2; ++i) transport.cameras[i] = (struct pico_camera_request_config){
        .crm_fd = c->cameras.session.crm_fd, .sync_fd = c->sync_fd, .ion_fd = c->ion_fd,
        .sensor_fd = c->cameras.session.cameras[i].sensor_fd, .ife_fd = c->cameras.session.ife_fd,
        .session = view.session, .link = view.cameras[i].link, .sensor = view.cameras[i].sensor,
        .ife = view.cameras[i].ife, .device_mmu = view.capabilities.device_mmu, .command_mmu = view.capabilities.cdm_mmu,
        .command_mapping = c->cameras.command_mapping, .ion_heap = c->ion_heap,
        .width = c->tracking.input.width[i], .height = c->tracking.input.height[i], .stride = c->stride[i],
        .camera_id = s->camera_ids[i], .generation = s->generation, .request_id = s->next_request, .synchronized = true};
    const struct pico_camera_transport_ops ops = {frame, invalidate};
    result = pico_camera_transport_create(&transport, &ops, s, &s->transport);
    if (result) return result;
    for (unsigned i = 0; i < 3; ++i) {
        s->subscriptions[i] = true;
        result = pico_camera_event_subscribe(s->event_fds[i], i == 1 ? 1 : 0, true);
        if (result) return result;
    }
    if (!pico_slam_runtime_start(s->runtime)) return -EIO;
    s->ready = true;
    return 0;
}

int pico_slam_live_stop(struct pico_slam_live *s)
{
    if (!s) return -EINVAL;
    if (s->stopped) return s->stop_error;
    s->stopped = true;
    pico_slam_runtime_stop(s->runtime);
    if (s->transport) pico_camera_transport_stop(s->transport);
    s->stop_error = s->rig ? pico_camera_rig_stop(s->rig) : 0;
    return s->stop_error;
}

static int fail(struct pico_slam_live *s, int error)
{
    s->failed = true;
    pico_slam_live_stop(s);
    return error;
}

static int refill(struct pico_slam_live *s, const int64_t offsets[2], int64_t now,
                  int64_t (*clock)(void *), void *clock_data)
{
    for (unsigned i = 0; i < s->queue_depth; ++i) {
        if (s->pending[i].request) continue;
        if (s->next_request > INT64_MAX) return -EOVERFLOW;
        if (clock) { now = clock(clock_data); if (now <= 0) return -EIO; }
        int result = pico_camera_transport_queue(s->transport, offsets, now);
        if (result) return result;
        s->pending[i].request = s->next_request++;
    }
    return 0;
}

static int poll_live(struct pico_slam_live *s, const int64_t offsets[2], int64_t now, size_t *poses,
                     int64_t (*clock)(void *), void *clock_data)
{
    if (!s || !poses) return -EINVAL;
    *poses = 0;
    if (!s->ready || s->failed || s->stopped) return fail(s, -ECANCELED);
    if (!offsets || offsets[0] < -1000000000 || offsets[0] > 1000000000 ||
        offsets[1] < -1000000000 || offsets[1] > 1000000000) return fail(s, -EINVAL);
    int result = pico_slam_runtime_poll(s->runtime, now, poses);
    if (result < 0) { *poses = 0; return fail(s, result); }
    if (!result) return 0;
    if (!s->streaming) {
        result = refill(s, offsets, now, clock, clock_data);
        if (!result) result = pico_camera_rig_start(s->rig);
        if (result) { *poses = 0; return fail(s, result); }
        s->streaming = true;
    }
    result = clock ? pico_camera_transport_poll_clock(s->transport, clock, clock_data) :
                     pico_camera_transport_poll(s->transport, now);
    if (result < 0 || s->failed) { *poses = 0; return fail(s, result < 0 ? result : -EIO); }
    if (clock) {
        now = clock(clock_data);
        if (now <= 0) { *poses = 0; return fail(s, -EIO); }
    }
    size_t delivered = 0;
    result = pico_slam_runtime_poll(s->runtime, now, &delivered);
    if (result != 1) { *poses = 0; return fail(s, result < 0 ? result : -EIO); }
    *poses += delivered;
    result = refill(s, offsets, now, clock, clock_data);
    if (result) { *poses = 0; return fail(s, result); }
    return 1;
}

int pico_slam_live_poll(struct pico_slam_live *s, const int64_t offsets[2], int64_t now, size_t *poses)
{ return poll_live(s, offsets, now, poses, NULL, NULL); }

struct poll_clock { int64_t (*read)(void *); void *data; int64_t last; };
static int64_t read_clock(void *data)
{
    struct poll_clock *clock = data;
    int64_t now = clock->read(clock->data);
    if (now <= 0 || now < clock->last) return 0;
    clock->last = now;
    return now;
}
int pico_slam_live_poll_clock(struct pico_slam_live *s, const int64_t offsets[2],
                              int64_t (*clock)(void *), void *clock_data, size_t *poses)
{
    if (!s || !poses) return -EINVAL;
    *poses = 0;
    if (!clock) return fail(s, -EINVAL);
    struct poll_clock current = {clock, clock_data, 0};
    int64_t now = read_clock(&current);
    if (now <= 0) return fail(s, -EIO);
    return poll_live(s, offsets, now, poses, read_clock, &current);
}

struct pico_slam_tracking *pico_slam_live_tracking(struct pico_slam_live *s)
{ return s ? pico_slam_runtime_tracking(s->runtime) : NULL; }

int pico_slam_live_quiesced(struct pico_slam_live *s)
{
    if (!s || !s->stopped) return -EINVAL;
    if (s->quiescence_attempted) return s->cleanup_error;
    s->quiescence_attempted = true;
    s->cleanup_error = s->stop_error;
    if (s->cleanup_error) return s->cleanup_error;
    if (s->transport) {
        s->cleanup_error = pico_camera_transport_quiesced(s->transport);
        if (!s->cleanup_error) s->cleanup_error = pico_camera_transport_destroy(&s->transport);
        if (s->cleanup_error) return s->cleanup_error;
    }
    for (unsigned i = 0; i < 3; ++i) if (s->subscriptions[i]) {
        int result = pico_camera_event_subscribe(s->event_fds[i], i == 1 ? 1 : 0, false);
        if (result && !s->cleanup_error) s->cleanup_error = result;
        s->subscriptions[i] = false;
    }
    if (s->cleanup_error) return s->cleanup_error;
    if (s->rig) s->cleanup_error = pico_camera_rig_quiesced(s->rig);
    s->quiesced = !s->cleanup_error;
    return s->cleanup_error;
}

int pico_slam_live_destroy(struct pico_slam_live **live)
{
    if (!live) return -EINVAL;
    struct pico_slam_live *s = *live;
    if (!s) return 0;
    if (!s->quiesced) return -EBUSY;
    if (s->release_attempted) return s->cleanup_error;
    s->release_attempted = true;
    s->cleanup_error = pico_camera_rig_release(&s->rig);
    if (!s->cleanup_error && !pico_slam_runtime_destroy(&s->runtime)) s->cleanup_error = -EBUSY;
    if (s->cleanup_error) return s->cleanup_error;
    free(s); *live = NULL;
    return 0;
}
