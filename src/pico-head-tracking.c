#include "pico-head-tracking.h"

#include <math/m_imu_3dof.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_SIZE 32

struct sample {
    struct xrt_vec3 vector;
    struct ssc_timestamp time;
    uint64_t ticks;
};

struct pico_head_tracking {
    struct pico_head_tracking_config config;
    struct m_imu_3dof fusion;
    pthread_mutex_t lock;
    struct sample queue[2][QUEUE_SIZE];
    unsigned count[2];
    uint64_t generation, last_ticks[2];
    int64_t last_time[2], last_received_ns, paired_ns, uncertainty_ns;
    bool active, valid;
};

static bool configuration(const struct pico_head_tracking_config *c)
{
    if (!c || !isfinite(c->acceleration_scale) || c->acceleration_scale <= 0 ||
        !isfinite(c->angular_velocity_scale) || c->angular_velocity_scale <= 0 ||
        c->max_age_ns <= 0 || c->max_age_ns > INT64_C(1000000000) ||
        c->max_gap_ns <= 0 || c->max_gap_ns > c->max_age_ns ||
        c->minimum_accuracy < 1 || c->minimum_accuracy > 3) return false;
    const float *m = c->sensor_to_head;
    for (unsigned i = 0; i < 9; i++) if (!isfinite(m[i])) return false;
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned j = 0; j < 3; j++) {
            double dot = 0;
            for (unsigned k = 0; k < 3; k++) dot += (double)m[3*i+k] * m[3*j+k];
            if (fabs(dot - (i == j ? 1 : 0)) > 0.0001) return false;
        }
    }
    double det = (double)m[0]*(m[4]*m[8]-m[5]*m[7]) -
                 (double)m[1]*(m[3]*m[8]-m[5]*m[6]) +
                 (double)m[2]*(m[3]*m[7]-m[4]*m[6]);
    return fabs(det - 1) <= 0.0001;
}

static bool current(struct ssc_timestamp time, int64_t now, int64_t max_age)
{
    return time.timestamp_ns > 0 && time.uncertainty_ns >= 0 &&
        time.uncertainty_ns <= time.timestamp_ns && time.uncertainty_ns <= max_age &&
        now >= time.timestamp_ns && now - time.timestamp_ns >= time.uncertainty_ns &&
        now - time.timestamp_ns <= max_age - time.uncertainty_ns;
}

static bool transform(const struct pico_head_tracking_config *c,
                      const struct ssc_imu_event *event, struct xrt_vec3 *result)
{
    double scale = event->sensor == SSC_IMU_ACCELERATION ? c->acceleration_scale : c->angular_velocity_scale;
    double values[3], norm = 0;
    for (unsigned i = 0; i < 3; i++) {
        values[i] = 0;
        for (unsigned j = 0; j < 3; j++) {
            if (!isfinite(event->vector.values[j])) return false;
            values[i] += (double)c->sensor_to_head[3*i+j] * event->vector.values[j];
        }
        values[i] *= scale;
        norm += values[i] * values[i];
    }
    if (!isfinite(norm) || norm > FLT_MAX / 32.0) return false;
    *result = (struct xrt_vec3){(float)values[0], (float)values[1], (float)values[2]};
    return true;
}

bool pico_head_tracking_create(const struct pico_head_tracking_config *config,
                               struct pico_head_tracking **tracking)
{
    if (!tracking || *tracking || !configuration(config)) return false;
    struct pico_head_tracking *t = calloc(1, sizeof(*t));
    if (!t) return false;
    if (pthread_mutex_init(&t->lock, NULL)) { free(t); return false; }
    t->config = *config;
    m_imu_3dof_init(&t->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_300MS);
    *tracking = t;
    return true;
}

bool pico_head_tracking_begin(struct pico_head_tracking *t, uint64_t generation)
{
    if (!t || !generation) return false;
    pthread_mutex_lock(&t->lock);
    bool accepted = generation > t->generation;
    if (accepted) {
        /* Monado's reset retains FIFO contents; a new stream needs empty history. */
        m_imu_3dof_close(&t->fusion);
        m_imu_3dof_init(&t->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_300MS);
        memset(t->count, 0, sizeof(t->count));
        memset(t->last_ticks, 0, sizeof(t->last_ticks));
        memset(t->last_time, 0, sizeof(t->last_time));
        t->last_received_ns = t->paired_ns = t->uncertainty_ns = 0;
        t->generation = generation;
        t->active = true;
        t->valid = false;
    }
    pthread_mutex_unlock(&t->lock);
    return accepted;
}

static void pop(struct pico_head_tracking *t, unsigned sensor)
{
    --t->count[sensor];
    memmove(t->queue[sensor], t->queue[sensor] + 1, t->count[sensor] * sizeof(struct sample));
}

static void pair(struct pico_head_tracking *t)
{
    while (t->count[0] && t->count[1]) {
        struct sample a = t->queue[0][0], g = t->queue[1][0];
        if (a.time.timestamp_ns != g.time.timestamp_ns) {
            pop(t, a.time.timestamp_ns < g.time.timestamp_ns ? 0 : 1);
            continue;
        }
        pop(t, 0); pop(t, 1);
        if (a.ticks != g.ticks) continue;
        int64_t timestamp = a.time.timestamp_ns;
        if (t->paired_ns && timestamp - t->paired_ns > t->config.max_gap_ns) {
            t->valid = t->active = false;
            return;
        }
        bool initialized = t->paired_ns != 0;
        m_imu_3dof_update(&t->fusion, (uint64_t)timestamp, &a.vector, &g.vector);
        const struct xrt_quat q = t->fusion.rot;
        double norm = (double)q.x*q.x + (double)q.y*q.y + (double)q.z*q.z + (double)q.w*q.w;
        if (!isfinite(norm) || fabs(norm - 1) > 0.001) {
            t->valid = t->active = false;
            return;
        }
        t->paired_ns = timestamp;
        t->uncertainty_ns = a.time.uncertainty_ns > g.time.uncertainty_ns ?
                            a.time.uncertainty_ns : g.time.uncertainty_ns;
        t->valid = initialized;
    }
}

bool pico_head_tracking_push(struct pico_head_tracking *t,
                             const struct ssc_imu_event *event, int64_t now_ns)
{
    if (!t || !event || (event->sensor != SSC_IMU_ACCELERATION && event->sensor != SSC_IMU_GYROSCOPE)) return false;
    struct xrt_vec3 vector;
    if (!current(event->time, now_ns, t->config.max_age_ns) ||
        event->vector.accuracy < t->config.minimum_accuracy || event->vector.accuracy > 3 ||
        !transform(&t->config, event, &vector)) return false;
    unsigned i = event->sensor;
    pthread_mutex_lock(&t->lock);
    bool accepted = t->active && event->generation == t->generation &&
        event->ticks > t->last_ticks[i] && event->time.timestamp_ns > t->last_time[i] &&
        now_ns >= t->last_received_ns;
    if (accepted) {
        if (t->count[i] == QUEUE_SIZE) pop(t, i);
        t->queue[i][t->count[i]++] = (struct sample){vector, event->time, event->ticks};
        t->last_ticks[i] = event->ticks;
        t->last_time[i] = event->time.timestamp_ns;
        t->last_received_ns = now_ns;
        pair(t);
    }
    pthread_mutex_unlock(&t->lock);
    return accepted;
}

bool pico_head_tracking_get(struct pico_head_tracking *t, int64_t now_ns,
                            struct xrt_space_relation *relation, int64_t *sample_time_ns)
{
    if (!relation || !sample_time_ns) return false;
    memset(relation, 0, sizeof(*relation));
    *sample_time_ns = 0;
    if (!t) return false;
    pthread_mutex_lock(&t->lock);
    struct ssc_timestamp time = {t->paired_ns, t->uncertainty_ns};
    bool valid = t->active && t->valid && current(time, now_ns, t->config.max_age_ns);
    if (valid) {
        relation->pose.orientation = t->fusion.rot;
        relation->relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;
        *sample_time_ns = t->paired_ns;
    }
    pthread_mutex_unlock(&t->lock);
    return valid;
}

void pico_head_tracking_disconnect(struct pico_head_tracking *t)
{
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    t->active = t->valid = false;
    memset(t->count, 0, sizeof(t->count));
    pthread_mutex_unlock(&t->lock);
}

void pico_head_tracking_destroy(struct pico_head_tracking **tracking)
{
    if (!tracking || !*tracking) return;
    struct pico_head_tracking *t = *tracking;
    *tracking = NULL;
    m_imu_3dof_close(&t->fusion);
    pthread_mutex_destroy(&t->lock);
    free(t);
}
