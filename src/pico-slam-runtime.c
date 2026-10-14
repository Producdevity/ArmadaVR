#include "pico-slam-runtime.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

struct pico_slam_runtime {
    struct ssc_imu_session *sensors;
    struct ssc_imu_session_config sensor_config;
    bool sensors_attempted;
    struct pico_slam_imu *queue;
    struct pico_slam_session *session;
    bool (*calibrate)(void *data, struct ssc_clock *clock);
    void *calibration_data;
    uint64_t generation, sensor_generation;
    int64_t offset, uncertainty, expires, last_poll;
    atomic_bool stopping, queue_started;
    bool started;
};

static void invalidate(void *data)
{
    struct pico_slam_runtime *s = data;
    atomic_store(&s->stopping, true);
    pico_slam_imu_stop(s->queue, s->generation);
    pico_slam_tracking_stop(pico_slam_session_tracking(s->session));
}

static bool calibrate(void *data, struct ssc_clock *clock)
{
    struct pico_slam_runtime *s = data;
    return !atomic_load(&s->stopping) && s->calibrate(s->calibration_data, clock);
}

static void raw_report(void *data, const struct ssc_imu_event *sample)
{
    (void)data;
    (void)sample;
}

static void corrected_report(void *data, const struct ssc_bias_sample *sample)
{
    struct pico_slam_runtime *s = data;
    if (atomic_load(&s->stopping)) return;
    struct timespec clock;
    if (!sample || !sample->raw.generation || clock_gettime(CLOCK_MONOTONIC, &clock) ||
        clock.tv_sec < 0 || clock.tv_nsec < 0 || clock.tv_nsec >= 1000000000 ||
        clock.tv_sec > (INT64_MAX - clock.tv_nsec) / INT64_C(1000000000)) {
        invalidate(s); return;
    }
    int64_t now = clock.tv_sec * INT64_C(1000000000) + clock.tv_nsec;
    if (!s->sensor_generation) {
        if (!pico_slam_imu_begin(s->queue, s->generation, now)) { invalidate(s); return; }
        s->sensor_generation = sample->raw.generation;
        atomic_store(&s->queue_started, true);
    }
    struct ssc_bias_sample value = *sample;
    value.raw.generation = s->generation;
    if (sample->raw.generation != s->sensor_generation ||
        !pico_slam_imu_push(s->queue, &value, now)) invalidate(s);
}

bool pico_slam_runtime_prepare(const struct pico_slam_runtime_config *c,
                              struct pico_slam_runtime **runtime)
{
    if (!c || !runtime || *runtime || !c->generation || !c->sensors.calibrate ||
        c->sensors.report || c->sensors.invalidate || c->sensors.bias.report ||
        c->sensors.bias.rigid_body[0] != 0 || c->sensors.bias.rigid_body[1] != 0 ||
        c->camera_uncertainty_ns < 0 || c->camera_uncertainty_ns > c->input.max_skew_ns / 2 ||
        c->clock_expires_ns <= 0) return false;
    struct pico_slam_runtime *s = calloc(1, sizeof(*s));
    if (!s) return false;
    atomic_init(&s->stopping, false);
    atomic_init(&s->queue_started, false);
    s->generation = c->generation;
    s->offset = c->camera_offset_ns; s->uncertainty = c->camera_uncertainty_ns;
    s->expires = c->clock_expires_ns;
    s->calibrate = c->sensors.calibrate; s->calibration_data = c->sensors.data;
    if (!pico_slam_imu_create(&c->imu, &s->queue) ||
        !pico_slam_session_create(&c->input, &c->tracking, &c->backend, c->backend_config, &s->session)) {
        pico_slam_imu_destroy(&s->queue);
        free(s);
        return false;
    }
    struct ssc_imu_session_config sensors = c->sensors;
    sensors.calibrate = calibrate; sensors.report = raw_report;
    sensors.invalidate = invalidate; sensors.bias.report = corrected_report; sensors.data = s;
    s->sensor_config = sensors;
    *runtime = s;
    return true;
}

bool pico_slam_runtime_start(struct pico_slam_runtime *s)
{
    if (!s || s->sensors_attempted || atomic_load(&s->stopping)) return false;
    s->sensors_attempted = true;
    if (!ssc_imu_session_create(&s->sensor_config, &s->sensors)) {
        invalidate(s);
        return false;
    }
    return true;
}

bool pico_slam_runtime_create(const struct pico_slam_runtime_config *c,
                              struct pico_slam_runtime **runtime)
{
    if (!pico_slam_runtime_prepare(c, runtime)) return false;
    if (pico_slam_runtime_start(*runtime)) return true;
    pico_slam_runtime_destroy(runtime);
    return false;
}

void pico_slam_runtime_stop(struct pico_slam_runtime *s)
{
    if (!s) return;
    invalidate(s);
    if (s->sensors) ssc_imu_session_stop(s->sensors);
    pico_slam_session_stop(s->session);
}

int pico_slam_runtime_poll(struct pico_slam_runtime *s, int64_t now, size_t *poses)
{
    if (!s || !poses) return -EINVAL;
    *poses = 0;
    if (atomic_load(&s->stopping) || now <= 0 || now < s->last_poll || now >= s->expires) {
        pico_slam_runtime_stop(s); return -ECANCELED;
    }
    s->last_poll = now;
    if (!atomic_load(&s->queue_started)) return 0;
    for (unsigned i = 0; i < 128; ++i) {
        struct xrt_imu_sample sample;
        int64_t uncertainty;
        int result = pico_slam_imu_pop(s->queue, s->generation, now, &sample, &uncertainty);
        if (result < 0) { pico_slam_runtime_stop(s); return result; }
        if (!result) break;
        if (!s->started) {
            if (!pico_slam_session_begin(s->session, s->generation, s->offset, s->uncertainty, s->expires, now)) {
                pico_slam_runtime_stop(s); return -EIO;
            }
            s->started = true;
        }
        if (atomic_load(&s->stopping) ||
            !pico_slam_session_imu(s->session, s->generation, &sample, uncertainty, now)) {
            pico_slam_runtime_stop(s); return -EIO;
        }
    }
    if (s->started && !pico_slam_session_poll(s->session, s->generation, now, poses)) {
        pico_slam_runtime_stop(s); return -EIO;
    }
    if (atomic_load(&s->stopping)) { pico_slam_runtime_stop(s); *poses = 0; return -ECANCELED; }
    return s->started ? 1 : 0;
}

bool pico_slam_runtime_captured(struct pico_slam_runtime *s, uint64_t generation,
                                const struct pico_camera_capture_frame *frame, int64_t now)
{
    if (!s || generation != s->generation || !s->started) return false;
    if (atomic_load(&s->stopping) ||
        !pico_slam_session_captured(s->session, generation, frame, now) || atomic_load(&s->stopping)) {
        pico_slam_runtime_stop(s); return false;
    }
    return true;
}

struct pico_slam_tracking *pico_slam_runtime_tracking(struct pico_slam_runtime *s)
{
    return s ? pico_slam_session_tracking(s->session) : NULL;
}

void pico_slam_runtime_get_status(struct pico_slam_runtime *s, struct ssc_imu_session_status *status)
{
    if (s && s->sensors) ssc_imu_session_get_status(s->sensors, status);
}

bool pico_slam_runtime_destroy(struct pico_slam_runtime **runtime)
{
    if (!runtime || !*runtime) return true;
    struct pico_slam_runtime *s = *runtime;
    if (!ssc_imu_session_destroy(&s->sensors)) return false;
    pico_slam_session_destroy(&s->session);
    pico_slam_imu_destroy(&s->queue);
    free(s);
    *runtime = NULL;
    return true;
}
