#include "pico-head-session.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct pico_head_session {
    struct ssc_imu_session *sensors;
    struct pico_head_tracking *tracking;
    bool (*calibrate)(void *data, struct ssc_clock *clock);
    void *calibration_data;
    uint64_t generation;
    atomic_bool stopping;
};

static bool calibrate(void *data, struct ssc_clock *clock)
{
    struct pico_head_session *s = data;
    return s->calibrate(s->calibration_data, clock);
}

static void raw_report(void *data, const struct ssc_imu_event *event)
{
    (void)data;
    (void)event;
}

static void corrected_report(void *data, const struct ssc_bias_sample *sample)
{
    struct pico_head_session *s = data;
    if (atomic_load(&s->stopping)) return;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0 ||
        now.tv_sec > (INT64_MAX - now.tv_nsec) / INT64_C(1000000000)) {
        pico_head_tracking_disconnect(s->tracking);
        return;
    }
    if (!s->generation) {
        if (!pico_head_tracking_begin(s->tracking, sample->raw.generation)) return;
        s->generation = sample->raw.generation;
    }
    struct ssc_imu_event event = sample->raw;
    memcpy(event.vector.values, sample->corrected, sizeof(event.vector.values));
    event.vector.accuracy = sample->accuracy;
    (void)pico_head_tracking_push(s->tracking, &event,
                                  now.tv_sec * INT64_C(1000000000) + now.tv_nsec);
}

static void invalidate(void *data)
{
    struct pico_head_session *s = data;
    atomic_store(&s->stopping, true);
    pico_head_tracking_disconnect(s->tracking);
}

bool pico_head_session_create(const struct ssc_imu_session_config *sensors,
                              const struct pico_head_tracking_config *tracking,
                              struct pico_head_session **session)
{
    if (!session || *session || !sensors || !sensors->calibrate ||
        sensors->report || sensors->invalidate || sensors->bias.report ||
        sensors->bias.rigid_body[0] != 0 || sensors->bias.rigid_body[1] != 0) return false;
    struct pico_head_session *s = calloc(1, sizeof(*s));
    if (!s) return false;
    if (!pico_head_tracking_create(tracking, &s->tracking)) { free(s); return false; }
    atomic_init(&s->stopping, false);
    s->calibrate = sensors->calibrate;
    s->calibration_data = sensors->data;
    struct ssc_imu_session_config config = *sensors;
    config.calibrate = calibrate;
    config.report = raw_report;
    config.invalidate = invalidate;
    config.bias.report = corrected_report;
    config.data = s;
    if (!ssc_imu_session_create(&config, &s->sensors)) {
        pico_head_tracking_destroy(&s->tracking);
        free(s);
        return false;
    }
    *session = s;
    return true;
}

bool pico_head_session_get(struct pico_head_session *s, int64_t now_ns,
                           struct xrt_space_relation *relation, int64_t *sample_time_ns)
{
    return pico_head_tracking_get(s && !atomic_load(&s->stopping) ? s->tracking : NULL, now_ns, relation, sample_time_ns);
}

void pico_head_session_get_status(struct pico_head_session *s,
                                  struct ssc_imu_session_status *status)
{
    if (s) ssc_imu_session_get_status(s->sensors, status);
}

void pico_head_session_stop(struct pico_head_session *s)
{
    if (!s) return;
    atomic_store(&s->stopping, true);
    ssc_imu_session_stop(s->sensors);
    pico_head_tracking_disconnect(s->tracking);
}

bool pico_head_session_destroy(struct pico_head_session **session)
{
    if (!session || !*session) return true;
    struct pico_head_session *s = *session;
    if (!ssc_imu_session_destroy(&s->sensors)) return false;
    pico_head_tracking_destroy(&s->tracking);
    free(s);
    *session = NULL;
    return true;
}
