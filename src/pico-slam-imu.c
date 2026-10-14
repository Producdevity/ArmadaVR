#include "pico-slam-imu.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SENSOR_CAPACITY 32
#define PAIR_CAPACITY 128

struct vector_sample {
    struct xrt_vec3_f64 value;
    struct ssc_timestamp time;
    uint64_t ticks;
};
struct paired_sample {
    struct xrt_imu_sample value;
    int64_t uncertainty;
};
struct pico_slam_imu {
    struct pico_slam_imu_config config;
    pthread_mutex_t lock;
    struct vector_sample pending[2][SENSOR_CAPACITY];
    struct paired_sample pairs[PAIR_CAPACITY];
    unsigned count[2], first_pair, pair_count;
    uint64_t generation, last_ticks[2];
    int64_t last_time[2], last_received, last_polled, began, paired, paired_uncertainty;
    bool active;
};

static bool configuration(const struct pico_slam_imu_config *c)
{
    if (!c || !isfinite(c->acceleration_scale) || c->acceleration_scale <= 0 ||
        !isfinite(c->angular_velocity_scale) || c->angular_velocity_scale <= 0 ||
        c->max_age_ns <= 0 || c->max_age_ns > INT64_C(1000000000) ||
        c->max_gap_ns <= 0 || c->max_gap_ns > c->max_age_ns ||
        c->minimum_accuracy < 1 || c->minimum_accuracy > 3) return false;
    const float *m = c->sensor_to_imu;
    for (unsigned i = 0; i < 9; ++i) if (!isfinite(m[i])) return false;
    for (unsigned i = 0; i < 3; ++i) for (unsigned j = 0; j < 3; ++j) {
        double dot = 0;
        for (unsigned k = 0; k < 3; ++k) dot += (double)m[3*i+k] * m[3*j+k];
        if (fabs(dot - (i == j ? 1 : 0)) > 0.0001) return false;
    }
    double det = (double)m[0]*(m[4]*m[8]-m[5]*m[7]) -
                 (double)m[1]*(m[3]*m[8]-m[5]*m[6]) +
                 (double)m[2]*(m[3]*m[7]-m[4]*m[6]);
    return fabs(det - 1) <= 0.0001;
}

static bool fresh(struct ssc_timestamp time, int64_t now, int64_t age)
{
    return time.timestamp_ns > 0 && time.uncertainty_ns >= 0 &&
        time.uncertainty_ns <= time.timestamp_ns && time.uncertainty_ns <= age &&
        now >= time.timestamp_ns && now - time.timestamp_ns >= time.uncertainty_ns &&
        now - time.timestamp_ns <= age - time.uncertainty_ns;
}

static void stop(struct pico_slam_imu *q)
{
    q->active = false;
    q->count[0] = q->count[1] = q->first_pair = q->pair_count = 0;
}

bool pico_slam_imu_create(const struct pico_slam_imu_config *c, struct pico_slam_imu **imu)
{
    if (!imu || *imu || !configuration(c)) return false;
    struct pico_slam_imu *q = calloc(1, sizeof(*q));
    if (!q) return false;
    if (pthread_mutex_init(&q->lock, NULL)) { free(q); return false; }
    q->config = *c;
    *imu = q;
    return true;
}

bool pico_slam_imu_begin(struct pico_slam_imu *q, uint64_t generation, int64_t now)
{
    if (!q || !generation || now <= 0) return false;
    pthread_mutex_lock(&q->lock);
    bool ok = generation > q->generation;
    if (ok) {
        stop(q);
        memset(q->last_ticks, 0, sizeof(q->last_ticks));
        memset(q->last_time, 0, sizeof(q->last_time));
        q->last_received = q->began = now;
        q->last_polled = 0;
        q->paired = q->paired_uncertainty = 0;
        q->generation = generation;
        q->active = true;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

static bool convert(const struct pico_slam_imu_config *c, const struct ssc_bias_sample *s,
                     struct xrt_vec3_f64 *vector)
{
    double values[3], norm = 0;
    double scale = s->raw.sensor == SSC_IMU_ACCELERATION ? c->acceleration_scale : c->angular_velocity_scale;
    for (unsigned i = 0; i < 3; ++i) {
        values[i] = 0;
        for (unsigned j = 0; j < 3; ++j) {
            if (!isfinite(s->corrected[j])) return false;
            values[i] += (double)c->sensor_to_imu[3*i+j] * s->corrected[j];
        }
        values[i] *= scale;
        norm += values[i] * values[i];
    }
    if (!isfinite(norm) || norm > FLT_MAX / 32.0) return false;
    *vector = (struct xrt_vec3_f64){values[0], values[1], values[2]};
    return true;
}

static void discard(struct pico_slam_imu *q, unsigned sensor)
{
    --q->count[sensor];
    memmove(q->pending[sensor], q->pending[sensor] + 1,
            q->count[sensor] * sizeof(q->pending[sensor][0]));
}

static bool pair(struct pico_slam_imu *q)
{
    while (q->count[0] && q->count[1]) {
        struct vector_sample a = q->pending[0][0], g = q->pending[1][0];
        if (a.time.timestamp_ns != g.time.timestamp_ns) {
            if ((a.time.timestamp_ns < g.time.timestamp_ns) != (a.ticks < g.ticks) || a.ticks == g.ticks)
                return false;
            discard(q, a.time.timestamp_ns < g.time.timestamp_ns ? 0 : 1);
            continue;
        }
        if (a.ticks != g.ticks || q->pair_count == PAIR_CAPACITY ||
            (q->paired && a.time.timestamp_ns - q->paired > q->config.max_gap_ns)) return false;
        struct paired_sample *p = &q->pairs[(q->first_pair + q->pair_count++) % PAIR_CAPACITY];
        p->value = (struct xrt_imu_sample){a.time.timestamp_ns, a.value, g.value};
        p->uncertainty = a.time.uncertainty_ns > g.time.uncertainty_ns ? a.time.uncertainty_ns : g.time.uncertainty_ns;
        q->paired = a.time.timestamp_ns;
        q->paired_uncertainty = p->uncertainty;
        discard(q, 0); discard(q, 1);
    }
    return true;
}

bool pico_slam_imu_push(struct pico_slam_imu *q, const struct ssc_bias_sample *s, int64_t now)
{
    if (!q || !s) return false;
    pthread_mutex_lock(&q->lock);
    bool ok = q->active && s->raw.generation == q->generation;
    if (ok) {
        const struct ssc_imu_event *r = &s->raw;
        struct xrt_vec3_f64 value;
        ok = (r->sensor == SSC_IMU_ACCELERATION || r->sensor == SSC_IMU_GYROSCOPE) &&
            s->accuracy >= q->config.minimum_accuracy && s->accuracy <= 3 &&
            fresh(r->time, now, q->config.max_age_ns) && now >= q->last_received &&
            convert(&q->config, s, &value);
        if (ok) {
            unsigned i = r->sensor;
            ok = r->ticks > q->last_ticks[i] && r->time.timestamp_ns > q->last_time[i] &&
                (!q->last_time[i] || r->time.timestamp_ns - q->last_time[i] <= q->config.max_gap_ns) &&
                q->count[i] < SENSOR_CAPACITY;
            if (ok) {
                q->pending[i][q->count[i]++] = (struct vector_sample){value, r->time, r->ticks};
                q->last_ticks[i] = r->ticks;
                q->last_time[i] = r->time.timestamp_ns;
                q->last_received = now;
                ok = pair(q);
            }
        }
        if (!ok) stop(q);
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

int pico_slam_imu_pop(struct pico_slam_imu *q, uint64_t generation, int64_t now,
                      struct xrt_imu_sample *sample, int64_t *uncertainty)
{
    if (!q || !sample || !uncertainty || !generation) return -EINVAL;
    pthread_mutex_lock(&q->lock);
    int result = 0;
    if (generation != q->generation) result = -ESTALE;
    else if (!q->active) result = -ECANCELED;
    else if (now <= 0 || now < q->last_polled || (now >= q->began && !q->paired && now - q->began > q->config.max_age_ns)) {
        stop(q);
        result = -ETIMEDOUT;
    } else if (q->paired) {
        struct paired_sample *p = &q->pairs[q->first_pair];
        struct ssc_timestamp time = q->pair_count ?
            (struct ssc_timestamp){p->value.timestamp_ns, p->uncertainty} :
            (struct ssc_timestamp){q->paired, q->paired_uncertainty};
        if (now < time.timestamp_ns || now - time.timestamp_ns < time.uncertainty_ns) {
            /* The consumer may have read its clock before the producer took the lock. */
            result = 0;
        } else if (!fresh(time, now, q->config.max_age_ns)) {
            stop(q);
            result = -ETIMEDOUT;
        } else if (q->pair_count) {
            *sample = p->value;
            *uncertainty = p->uncertainty;
            q->first_pair = (q->first_pair + 1) % PAIR_CAPACITY;
            --q->pair_count;
            result = 1;
        }
    }
    if (q->active && generation == q->generation) q->last_polled = now;
    pthread_mutex_unlock(&q->lock);
    return result;
}

void pico_slam_imu_stop(struct pico_slam_imu *q, uint64_t generation)
{
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    if (generation == q->generation) stop(q);
    pthread_mutex_unlock(&q->lock);
}

void pico_slam_imu_destroy(struct pico_slam_imu **imu)
{
    if (!imu || !*imu) return;
    struct pico_slam_imu *q = *imu;
    *imu = NULL;
    pthread_mutex_destroy(&q->lock);
    free(q);
}
