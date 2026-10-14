#include "pico-slam-tracking.h"
#include <math/m_api.h>
#include <math/m_relation_history.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct pico_slam_tracking {
    struct pico_slam_tracking_config config;
    struct m_relation_history *history;
    pthread_mutex_t lock;
    uint64_t generation;
    int64_t timestamp_ns, received_ns, uncertainty_ns;
    unsigned samples;
    bool active;
};

static bool pose_valid(const struct xrt_pose *p)
{
    if (!p) return false;
    struct xrt_quat q = p->orientation;
    struct xrt_vec3 v = p->position;
    double norm = (double)q.x*q.x + (double)q.y*q.y + (double)q.z*q.z + (double)q.w*q.w;
    return isfinite(norm) && fabs(norm - 1) <= 0.0001 &&
        isfinite(v.x) && isfinite(v.y) && isfinite(v.z) &&
        fabsf(v.x) <= 1000000 && fabsf(v.y) <= 1000000 && fabsf(v.z) <= 1000000;
}

static bool current(int64_t timestamp, int64_t uncertainty, int64_t now, int64_t age)
{
    return timestamp > 0 && uncertainty >= 0 && uncertainty <= age &&
        now >= timestamp && now - timestamp >= uncertainty && now - timestamp <= age - uncertainty;
}

bool pico_slam_tracking_create(const struct pico_slam_tracking_config *c, struct pico_slam_tracking **tracking)
{
    if (!tracking || *tracking || !c || !pose_valid(&c->openxr_from_slam) || !pose_valid(&c->imu_from_head) ||
        c->max_age_ns <= 0 || c->max_age_ns > 1000000000 || c->max_gap_ns <= 0 || c->max_gap_ns > c->max_age_ns ||
        c->max_prediction_ns < 0 || c->max_prediction_ns > 50000000) return false;
    struct pico_slam_tracking *t = calloc(1, sizeof(*t));
    if (!t) return false;
    if (pthread_mutex_init(&t->lock, NULL)) { free(t); return false; }
    t->config = *c;
    m_relation_history_create(&t->history);
    *tracking = t;
    return true;
}

bool pico_slam_tracking_begin(struct pico_slam_tracking *t, uint64_t generation)
{
    if (!t || !generation) return false;
    pthread_mutex_lock(&t->lock);
    bool accepted = generation > t->generation;
    if (accepted) {
        m_relation_history_clear(t->history);
        t->generation = generation;
        t->timestamp_ns = t->received_ns = t->uncertainty_ns = 0;
        t->samples = 0;
        t->active = true;
    }
    pthread_mutex_unlock(&t->lock);
    return accepted;
}

bool pico_slam_tracking_push(struct pico_slam_tracking *t, uint64_t generation,
                             const struct xrt_pose *pose, int64_t timestamp, int64_t uncertainty, int64_t now)
{
    if (!t) return false;
    pthread_mutex_lock(&t->lock);
    bool accepted = t->active && generation == t->generation;
    if (accepted) {
        accepted = pose_valid(pose) && current(timestamp, uncertainty, now, t->config.max_age_ns) &&
            now >= t->received_ns && timestamp > t->timestamp_ns;
        if (accepted && t->samples)
            accepted = timestamp - t->timestamp_ns <= t->config.max_gap_ns;
        if (accepted) {
            struct xrt_pose world_imu;
            struct xrt_space_relation relation = {0};
            math_pose_transform(&t->config.openxr_from_slam, pose, &world_imu);
            math_pose_transform(&world_imu, &t->config.imu_from_head, &relation.pose);
            relation.relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
                XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
                XRT_SPACE_RELATION_POSITION_TRACKED_BIT;
            accepted = pose_valid(&relation.pose) &&
                m_relation_history_push_with_motion_estimation(t->history, &relation, timestamp);
            if (accepted) {
                t->timestamp_ns = timestamp;
                t->received_ns = now;
                t->uncertainty_ns = uncertainty;
                if (t->samples < 2) ++t->samples;
            }
        }
        if (!accepted) t->active = false;
    }
    pthread_mutex_unlock(&t->lock);
    return accepted;
}

bool pico_slam_tracking_get(struct pico_slam_tracking *t, int64_t now, int64_t at,
                            struct xrt_space_relation *relation, int64_t *sample_time)
{
    if (!relation || !sample_time) return false;
    memset(relation, 0, sizeof(*relation));
    relation->pose.orientation.w = 1;
    *sample_time = 0;
    if (!t || at <= 0) return false;
    pthread_mutex_lock(&t->lock);
    bool valid = t->active && t->samples >= 2 && now >= t->received_ns &&
        current(t->timestamp_ns, t->uncertainty_ns, now, t->config.max_age_ns);
    if (valid && at > t->timestamp_ns)
        valid = t->uncertainty_ns <= t->config.max_prediction_ns &&
            at - t->timestamp_ns <= t->config.max_prediction_ns - t->uncertainty_ns;
    if (valid && at < now) valid = now - at <= t->config.max_age_ns;
    if (valid) {
        enum m_relation_history_result r = m_relation_history_get(t->history, at, relation);
        valid = r == M_RELATION_HISTORY_RESULT_EXACT || r == M_RELATION_HISTORY_RESULT_INTERPOLATED ||
                r == M_RELATION_HISTORY_RESULT_PREDICTED;
        if (valid) valid = pose_valid(&relation->pose) &&
            isfinite(relation->linear_velocity.x) && isfinite(relation->linear_velocity.y) &&
            isfinite(relation->linear_velocity.z) && isfinite(relation->angular_velocity.x) &&
            isfinite(relation->angular_velocity.y) && isfinite(relation->angular_velocity.z);
    }
    if (valid) *sample_time = t->timestamp_ns;
    else {
        memset(relation, 0, sizeof(*relation));
        relation->pose.orientation.w = 1;
    }
    pthread_mutex_unlock(&t->lock);
    return valid;
}

void pico_slam_tracking_stop(struct pico_slam_tracking *t)
{
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    t->active = false;
    m_relation_history_clear(t->history);
    pthread_mutex_unlock(&t->lock);
}

void pico_slam_tracking_destroy(struct pico_slam_tracking **tracking)
{
    if (!tracking || !*tracking) return;
    struct pico_slam_tracking *t = *tracking;
    *tracking = NULL;
    m_relation_history_destroy(&t->history);
    pthread_mutex_destroy(&t->lock);
    free(t);
}
