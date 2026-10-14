#ifndef PICO_SLAM_FIXTURE_H
#define PICO_SLAM_FIXTURE_H
#include "pico-slam-runtime.h"
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_t owner;
static void on_owner(void) { assert(pthread_equal(owner, pthread_self())); }
static unsigned created, destroyed, stopped, pose_created, pose_destroyed, frames, imus;
static int fault;
struct vit_tracker { vit_pose_data_t pending; bool available, started; unsigned next_camera; };
struct vit_pose { vit_pose_data_t data; };
static vit_result_t version(uint32_t *a,uint32_t *b,uint32_t *c) {on_owner();*a=2;*b=0;*c=1;return VIT_SUCCESS;}
static vit_result_t create(const vit_config_t *c,vit_tracker_t **t)
{
    on_owner();assert(c->cam_count==2 && c->imu_count==1 && !strcmp(c->file,"explicit-calibration.toml"));
    if(fault==1)return VIT_ERROR_ALLOCATION_FAILURE;
    *t=calloc(1,sizeof(**t));assert(*t);++created;return VIT_SUCCESS;
}
static void destroy(vit_tracker_t *t){on_owner();assert(!t->started);++destroyed;free(t);}
static vit_result_t format(const vit_tracker_t *t,vit_image_format_t f,bool *supported)
{on_owner();assert(t && f==VIT_IMAGE_FORMAT_L8);*supported=fault!=2;return VIT_SUCCESS;}
static vit_result_t start(vit_tracker_t *t){on_owner();if(fault==3)return VIT_ERROR_INVALID_VALUE;t->started=true;return VIT_SUCCESS;}
static vit_result_t stop(vit_tracker_t *t){on_owner();t->started=false;++stopped;return VIT_SUCCESS;}
static vit_result_t imu(vit_tracker_t *t,const vit_imu_sample_t *s)
{on_owner();assert(t->started && s->ay==9.8f && s->wz==0.1f);++imus;return fault==4?VIT_ERROR_ALLOCATION_FAILURE:VIT_SUCCESS;}
static vit_result_t image(vit_tracker_t *t,const vit_img_sample_t *s)
{
    on_owner();assert(t->started && s->cam_index==t->next_camera++%2 && s->format==VIT_IMAGE_FORMAT_L8);
    assert(s->size==6 && s->width==3 && s->height==2 && s->stride==3 && !memcmp(s->data,"abcdef",6));
    ++frames;if(fault==5)return VIT_ERROR_ALLOCATION_FAILURE;
    if(s->cam_index==1){assert(!t->available);t->pending=(vit_pose_data_t){.timestamp=s->timestamp,.ow=1,.px=0.2};t->available=true;}
    return VIT_SUCCESS;
}
static vit_result_t pop(vit_tracker_t *t,vit_pose_t **p)
{
    on_owner();*p=NULL;if(fault==6)return VIT_ERROR_INVALID_VALUE;
    if(t->available){*p=malloc(sizeof(**p));assert(*p);(*p)->data=t->pending;t->available=false;++pose_created;}
    return VIT_SUCCESS;
}
static vit_result_t pose_data(const vit_pose_t *p,vit_pose_data_t *d)
{on_owner();*d=p->data;if(fault==8)d->px=NAN;return fault==7?VIT_ERROR_INVALID_VALUE:VIT_SUCCESS;}
static void pose_destroy(vit_pose_t *p){on_owner();++pose_destroyed;free(p);}

struct ssc_imu_session {
    struct ssc_imu_session_config config;
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned command;
    bool complete, stopping;
    struct ssc_bias_sample sample;
    int64_t time;
};
static struct ssc_imu_session *mock;
static _Thread_local int64_t worker_time;
static unsigned sensor_created, sensor_destroyed;
static bool fail_create;
int clock_gettime(clockid_t id, struct timespec *value)
{
    assert(id == CLOCK_MONOTONIC && worker_time > 0);
    value->tv_sec = worker_time / 1000000000;
    value->tv_nsec = worker_time % 1000000000;
    return 0;
}
static void *worker(void *data)
{
    struct ssc_imu_session *s = data;
    pthread_mutex_lock(&s->lock);
    for (;;) {
        while (!s->command) pthread_cond_wait(&s->changed, &s->lock);
        unsigned command = s->command;
        struct ssc_bias_sample sample = s->sample;
        worker_time = s->time;
        pthread_mutex_unlock(&s->lock);
        if (command == 1) s->config.bias.report(s->config.data, &sample);
        else if (command == 2 || command == 3) s->config.invalidate(s->config.data);
        else if (command == 4) {
            struct ssc_clock clock;
            assert(s->config.calibrate(s->config.data, &clock));
            s->config.report(s->config.data, &sample.raw);
        }
        pthread_mutex_lock(&s->lock);
        s->command = 0; s->complete = true;
        pthread_cond_broadcast(&s->changed);
        if (command == 3) break;
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}
static void send(unsigned command, const struct ssc_bias_sample *sample, int64_t now)
{
    assert(mock);
    pthread_mutex_lock(&mock->lock);
    mock->command = command; mock->complete = false; mock->time = now;
    if (sample) mock->sample = *sample;
    pthread_cond_broadcast(&mock->changed);
    while (!mock->complete) pthread_cond_wait(&mock->changed, &mock->lock);
    pthread_mutex_unlock(&mock->lock);
}
bool ssc_imu_session_create(const struct ssc_imu_session_config *c, struct ssc_imu_session **out)
{
    assert(!mock && !*out && c->bias.report && c->invalidate && c->report && c->calibrate);
    if (fail_create) return false;
    mock = calloc(1, sizeof(*mock)); assert(mock);
    mock->config = *c;
    assert(!pthread_mutex_init(&mock->lock, NULL));
    assert(!pthread_cond_init(&mock->changed, NULL));
    assert(!pthread_create(&mock->worker, NULL, worker, mock));
    *out = mock; ++sensor_created;
    return true;
}
void ssc_imu_session_get_status(struct ssc_imu_session *s, struct ssc_imu_session_status *status)
{
    assert(s == mock);
    memset(status, 0, sizeof(*status)); status->stopping = s->stopping;
}
void ssc_imu_session_stop(struct ssc_imu_session *s)
{
    on_owner(); assert(s == mock); s->stopping = true;
}
bool ssc_imu_session_destroy(struct ssc_imu_session **s)
{
    if (!s || !*s) return true;
    on_owner(); assert(*s == mock);
    send(3, NULL, 1);
    assert(!pthread_join(mock->worker, NULL));
    pthread_cond_destroy(&mock->changed); pthread_mutex_destroy(&mock->lock);
    free(mock); mock = NULL; *s = NULL; ++sensor_destroyed;
    return true;
}
static bool calibrate(void *data, struct ssc_clock *clock)
{
    assert(data == &owner);
    memset(clock, 0, sizeof(*clock)); clock->initialized = true;
    return true;
}
static void paired(int64_t timestamp, int64_t now, uint64_t generation)
{
    for (unsigned i = 0; i < 2; ++i) {
        struct ssc_bias_sample s = {0};
        s.raw.sensor = i; s.raw.time.timestamp_ns = timestamp;
        s.raw.generation = generation; s.raw.ticks = timestamp; s.accuracy = 3;
        s.corrected[i ? 2 : 1] = i ? 0.1f : 9.8f;
        send(1, &s, now);
    }
}
static struct pico_slam_runtime_config fixture_config(void)
{
    return (struct pico_slam_runtime_config){
        .sensors = {.calibrate=calibrate, .data=&owner, .bias.rigid_body={0,0}},
        .imu = {.sensor_to_imu={1,0,0,0,1,0,0,0,1}, .acceleration_scale=1, .angular_velocity_scale=1,
                .max_age_ns=100000000, .max_gap_ns=10000000, .minimum_accuracy=2},
        .input = {.count=2,.camera_ids={0,1},.width={3,3},.height={2,2},
                  .max_age_ns=100000000,.max_gap_ns=10000000,.max_skew_ns=100000},
        .tracking = {.openxr_from_slam.orientation.w=1,.imu_from_head.orientation.w=1,
                     .max_age_ns=100000000,.max_gap_ns=10000000,.max_prediction_ns=50000000},
        .backend = {version,create,destroy,format,start,stop,imu,image,pop,pose_data,pose_destroy},
        .backend_config = "explicit-calibration.toml", .generation=55, .clock_expires_ns=5000000000,
    };
}
#endif
