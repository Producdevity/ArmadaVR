#define _POSIX_C_SOURCE 200809L
#include "quest-slam-runtime.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

struct quest_slam_runtime {
    struct quest_imu_stream_config config;
    struct quest_imu_timing timing;
    struct quest_imu_stream *sensors;
    struct pico_slam_session *session;
    int64_t imu_uncertainty, offset, camera_uncertainty, expires, last_clock;
    bool attempted, started;
    int error;
};

static void invalidate(struct quest_slam_runtime *s, int error)
{
    if (!s->error) s->error = error;
    pico_slam_tracking_stop(pico_slam_session_tracking(s->session));
}

static bool clock_now(struct quest_slam_runtime *s, int64_t *now)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 ||
        t.tv_nsec >= 1000000000 || t.tv_sec > (INT64_MAX - t.tv_nsec) / INT64_C(1000000000)) {
        invalidate(s, -ERANGE); return false;
    }
    *now = t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
    if (*now <= 0 || *now < s->last_clock || *now >= s->expires) {
        invalidate(s, -ETIMEDOUT); return false;
    }
    s->last_clock = *now;
    return !s->error;
}

static void sensor_failed(void *data, uint64_t generation, int error)
{
    struct quest_slam_runtime *s = data;
    if (generation == s->config.generation) invalidate(s, error);
}

static bool sample(void *data, uint64_t generation, const struct quest_imu_sample *value)
{
    struct quest_slam_runtime *s = data;
    int64_t now;
    if (!value || generation != s->config.generation || !clock_now(s, &now)) return false;
    if (!s->started) {
        if (!pico_slam_session_begin(s->session, generation, s->offset, s->camera_uncertainty, s->expires, now))
            return false;
        s->started = true;
    }
    struct xrt_imu_sample input = {.timestamp_ns = value->timestamp_ns,
        .accel_m_s2 = {value->sample.acceleration_m_s2[0], value->sample.acceleration_m_s2[1], value->sample.acceleration_m_s2[2]},
        .gyro_rad_secs = {value->sample.angular_velocity_rad_s[0], value->sample.angular_velocity_rad_s[1], value->sample.angular_velocity_rad_s[2]}};
    return pico_slam_session_imu(s->session, generation, &input, s->imu_uncertainty, now);
}

bool quest_slam_runtime_prepare(const struct quest_slam_runtime_config *c,
                                struct quest_slam_runtime **runtime)
{
    if (!c || !runtime || *runtime || !c->sensors.generation || !c->sensors.imu.timing ||
        c->sensors.sample || c->sensors.invalidated || c->sensors.userdata ||
        c->sensors.max_gap_ns <= 0 || c->sensors.max_gap_ns > c->sensors.imu.max_age_ns ||
        c->sensors.imu.max_age_ns > INT64_C(1000000000) ||
        c->imu_uncertainty_ns < 0 || c->imu_uncertainty_ns > c->input.max_age_ns ||
        c->camera_uncertainty_ns < 0 || c->camera_uncertainty_ns > c->input.max_skew_ns / 2 ||
        c->clock_expires_ns <= 0) return false;
    struct quest_imu check = {0};
    if (!quest_imu_init(&check, &c->sensors.imu)) return false;
    struct quest_slam_runtime *s = calloc(1, sizeof(*s));
    if (!s) return false;
    s->config = c->sensors;
    s->timing = *c->sensors.imu.timing;
    s->config.imu.timing = &s->timing;
    s->config.userdata = s; s->config.sample = sample; s->config.invalidated = sensor_failed;
    s->imu_uncertainty = c->imu_uncertainty_ns;
    s->offset = c->camera_offset_ns; s->camera_uncertainty = c->camera_uncertainty_ns;
    s->expires = c->clock_expires_ns;
    if (!pico_slam_session_create(&c->input, &c->tracking, &c->backend, c->backend_config, &s->session)) {
        free(s); return false;
    }
    *runtime = s;
    return true;
}

bool quest_slam_runtime_start(struct quest_slam_runtime *s)
{
    if (!s || s->attempted || s->error) return false;
    s->attempted = true;
    int64_t now;
    if (!clock_now(s, &now) || !quest_imu_stream_open(&s->config, &s->sensors)) {
        invalidate(s, -EIO); return false;
    }
    return true;
}

void quest_slam_runtime_stop(struct quest_slam_runtime *s)
{
    if (!s) return;
    invalidate(s, -ECANCELED);
    quest_imu_stream_stop(s->sensors);
    pico_slam_session_stop(s->session);
}

int quest_slam_runtime_poll(struct quest_slam_runtime *s, size_t *poses)
{
    if (!s || !poses) return -EINVAL;
    *poses = 0;
    if (!s->attempted && !s->error) return -EAGAIN;
    int64_t now;
    if (!clock_now(s, &now)) goto fail;
    int result = quest_imu_stream_poll(s->sensors);
    if (result < 0) { invalidate(s, result); goto fail; }
    if (!clock_now(s, &now)) goto fail;
    if (s->started && !pico_slam_session_poll(s->session, s->config.generation, now, poses)) {
        invalidate(s, -EIO); goto fail;
    }
    if (!clock_now(s, &now)) goto fail;
    return s->started ? 1 : 0;
fail:
    *poses = 0;
    quest_slam_runtime_stop(s);
    return s->error;
}

bool quest_slam_runtime_camera(struct quest_slam_runtime *s, uint64_t generation,
                               const struct pico_slam_mono8_frame *frame)
{
    if (!s || generation != s->config.generation || !s->started) return false;
    int64_t now;
    if (!clock_now(s, &now) || !pico_slam_session_mono8(s->session, generation, frame, now) ||
        !clock_now(s, &now)) {
        quest_slam_runtime_stop(s); return false;
    }
    return true;
}

bool quest_slam_runtime_get_relation(struct quest_slam_runtime *s, int64_t now,
                                     int64_t at, struct xrt_space_relation *relation,
                                     int64_t *sample_time)
{
    return pico_slam_tracking_get(s ? pico_slam_session_tracking(s->session) : NULL,
                                   now, at, relation, sample_time);
}

void quest_slam_runtime_destroy(struct quest_slam_runtime **runtime)
{
    if (!runtime || !*runtime) return;
    struct quest_slam_runtime *s = *runtime;
    quest_slam_runtime_stop(s);
    quest_imu_stream_destroy(&s->sensors);
    pico_slam_session_destroy(&s->session);
    free(s); *runtime = NULL;
}
