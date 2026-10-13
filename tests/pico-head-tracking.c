#include "pico-head-tracking.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const struct pico_head_tracking_config config = {
    .sensor_to_head = {1,0,0, 0,1,0, 0,0,1},
    .acceleration_scale = 1, .angular_velocity_scale = 1,
    .max_age_ns = 100000000, .max_gap_ns = 20000000, .minimum_accuracy = 1,
};
static unsigned pairs;

static struct ssc_imu_event event(unsigned index, unsigned sensor, unsigned axis, float speed)
{
    struct ssc_imu_event e = {
        .sensor = sensor, .generation = 1, .ticks = 19200000 + (uint64_t)index * 19200,
        .time = {INT64_C(1000000000) + (int64_t)index * 1000000, 10},
        .vector = {.accuracy = 3},
    };
    if (sensor == SSC_IMU_ACCELERATION) e.vector.values[1] = 9.82f;
    else e.vector.values[axis] = speed;
    return e;
}

static void push_pair(struct pico_head_tracking *t, unsigned index, uint64_t generation,
                      unsigned axis, float speed)
{
    for (unsigned sensor = 0; sensor < 2; sensor++) {
        struct ssc_imu_event e = event(index, sensor, axis, speed);
        e.generation = generation;
        assert(pico_head_tracking_push(t, &e, e.time.timestamp_ns + 10));
    }
    pairs++;
}

static struct xrt_space_relation get(struct pico_head_tracking *t, unsigned index, bool valid)
{
    struct xrt_space_relation relation;
    memset(&relation, 0xa5, sizeof(relation));
    int64_t time = -1;
    assert(pico_head_tracking_get(t, INT64_C(1000000010) + (int64_t)index * 1000000, &relation, &time) == valid);
    if (valid) {
        assert(time == INT64_C(1000000000) + (int64_t)index * 1000000);
        assert(relation.relation_flags == (XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT));
        assert(!relation.pose.position.x && !relation.pose.position.y && !relation.pose.position.z);
    } else {
        struct xrt_space_relation empty = {0};
        assert(!memcmp(&empty, &relation, sizeof(empty)) && time == 0);
    }
    return relation;
}

static void quaternion(struct xrt_quat q, double x, double y, double z, double w)
{
    assert(fabs(q.x-x)<0.0001 && fabs(q.y-y)<0.0001 && fabs(q.z-z)<0.0001 && fabs(q.w-w)<0.0001);
}

static void rotations(void)
{
    for (unsigned axis = 0; axis < 3; axis++) {
        struct pico_head_tracking *t = NULL;
        assert(pico_head_tracking_create(&config, &t));
        get(t, 0, false);
        assert(pico_head_tracking_begin(t, 1));
        for (unsigned i = 0; i <= 1000; i++) {
            push_pair(t, i, 1, axis, (float)(acos(-1.0)/2));
            if (!i) get(t, i, false);
        }
        struct xrt_quat q = get(t, 1000, true).pose.orientation;
        double v = sqrt(0.5);
        quaternion(q, axis==0?v:0, axis==1?v:0, axis==2?v:0, v);
        pico_head_tracking_destroy(&t); assert(!t);
    }
    struct pico_head_tracking_config c = config;
    float matrix[] = {0,1,0, -1,0,0, 0,0,1};
    memcpy(c.sensor_to_head,matrix,sizeof(matrix));
    c.angular_velocity_scale=acos(-1.0)/180; c.acceleration_scale=9.82;
    struct pico_head_tracking *t = NULL;
    assert(pico_head_tracking_create(&c,&t) && pico_head_tracking_begin(t,1));
    for (unsigned i=0;i<=1000;i++) push_pair(t,i,1,0,90);
    quaternion(get(t,1000,true).pose.orientation,0,-sqrt(0.5),0,sqrt(0.5));
    pico_head_tracking_destroy(&t);
}

static void refusals(void)
{
    struct pico_head_tracking *t = NULL;
    for (unsigned i=0;i<12;i++) {
        struct pico_head_tracking_config c=config;
        switch(i) {
        case 0: memset(c.sensor_to_head,0,sizeof(c.sensor_to_head));break;
        case 1: c.sensor_to_head[0]=-1;break;
        case 2: c.sensor_to_head[1]=0.5f;break;
        case 3: c.sensor_to_head[0]=NAN;break;
        case 4: c.acceleration_scale=0;break;
        case 5: c.angular_velocity_scale=-1;break;
        case 6: c.angular_velocity_scale=INFINITY;break;
        case 7: c.max_age_ns=INT64_MAX;break;
        case 8: c.max_gap_ns=0;break;
        case 9: c.max_gap_ns=c.max_age_ns+1;break;
        case 10: c.minimum_accuracy=0;break;
        case 11: c.minimum_accuracy=4;break;
        }
        assert(!pico_head_tracking_create(&c,&t) && !t);
    }
    assert(!pico_head_tracking_create(NULL,&t));
    assert(pico_head_tracking_create(&config,&t));
    assert(!pico_head_tracking_create(&config,&t));
    assert(!pico_head_tracking_begin(t,0));
    assert(pico_head_tracking_begin(t,1));
    push_pair(t,0,1,1,0);push_pair(t,1,1,1,0);
    for(unsigned i=0;i<12;i++) {
        struct ssc_imu_event e=event(2,0,0,0);
        int64_t now=e.time.timestamp_ns+10;
        switch(i) {
        case 0:e.sensor=2;break;
        case 1:e.generation=2;break;
        case 2:e.vector.accuracy=0;break;
        case 3:e.vector.accuracy=4;break;
        case 4:e.vector.values[0]=NAN;break;
        case 5:e.vector.values[0]=FLT_MAX;break;
        case 6:e.time.timestamp_ns=-1;break;
        case 7:e.time.uncertainty_ns=-1;break;
        case 8:e.time.uncertainty_ns=INT64_MAX;break;
        case 9:now=e.time.timestamp_ns;break;
        case 10:now+=config.max_age_ns;break;
        case 11:e.ticks=19200000;break;
        }
        assert(!pico_head_tracking_push(t,&e,now));
        quaternion(get(t,1,true).pose.orientation,0,0,0,1);
    }
    struct xrt_space_relation r;int64_t when;
    assert(!pico_head_tracking_get(t,1001000000,&r,&when));
    assert(!pico_head_tracking_get(t,1101000001,&r,&when));
    assert(!pico_head_tracking_get(t,INT64_MIN,&r,&when));
    push_pair(t,30,1,1,1); get(t,30,false);
    struct ssc_imu_event e=event(31,0,0,0);
    assert(!pico_head_tracking_push(t,&e,e.time.timestamp_ns+10));
    assert(!pico_head_tracking_begin(t,1));
    assert(pico_head_tracking_begin(t,2));
    push_pair(t,31,2,1,0); get(t,31,false);
    push_pair(t,32,2,1,0); quaternion(get(t,32,true).pose.orientation,0,0,0,1);
    pico_head_tracking_disconnect(t);get(t,32,false);
    assert(!pico_head_tracking_begin(t,2));
    pico_head_tracking_destroy(&t);pico_head_tracking_destroy(&t);
}

static void batching(void)
{
    for(unsigned batch=1;batch<=64;batch++) {
        struct pico_head_tracking *t=NULL;
        assert(pico_head_tracking_create(&config,&t) && pico_head_tracking_begin(t,1));
        for(unsigned sensor=0;sensor<2;sensor++)
            for(unsigned i=0;i<batch;i++) {
                struct ssc_imu_event e=event(i,sensor,1,1);
                assert(pico_head_tracking_push(t,&e,1000000010+(int64_t)(batch-1)*1000000));
            }
        struct xrt_space_relation r=get(t,batch-1,batch>1);
        if(batch>1) {
            double angle=((batch>32?32:batch)-1)*0.001;
            quaternion(r.pose.orientation,0,sin(angle/2),0,cos(angle/2));
        }
        pico_head_tracking_destroy(&t);
    }
    struct pico_head_tracking *t=NULL;
    assert(pico_head_tracking_create(&config,&t) && pico_head_tracking_begin(t,1));
    struct ssc_imu_event a=event(0,0,1,1),g=event(0,1,1,1);
    g.ticks++;
    assert(pico_head_tracking_push(t,&a,1000000010));
    assert(pico_head_tracking_push(t,&g,1000000010));
    get(t,0,false);
    push_pair(t,1,1,1,1);get(t,1,false);
    push_pair(t,2,1,1,1);get(t,2,true);
    pico_head_tracking_destroy(&t);
}

struct concurrent {struct pico_head_tracking *t;atomic_bool done;};
static void *reader(void *data)
{
    struct concurrent *c=data;
    while(!atomic_load(&c->done)) {
        struct xrt_space_relation r;int64_t time;
        bool valid=pico_head_tracking_get(c->t,10001000000,&r,&time);
        if(valid) assert(r.relation_flags==(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT|XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT));
        else assert(!r.relation_flags && !time);
    }
    return NULL;
}

int main(void)
{
    rotations();refusals();batching();
    struct concurrent c={0};pthread_t thread;
    assert(pico_head_tracking_create(&config,&c.t) && pico_head_tracking_begin(c.t,1));
    assert(!pthread_create(&thread,NULL,reader,&c));
    for(unsigned i=0;i<10000;i++) push_pair(c.t,i,1,1,0.2f);
    atomic_store(&c.done,true);assert(!pthread_join(thread,NULL));
    for(unsigned gen=2;gen<=101;gen++) {
        pico_head_tracking_disconnect(c.t);
        assert(pico_head_tracking_begin(c.t,gen));
        push_pair(c.t,0,gen,1,0);push_pair(c.t,1,gen,1,0);
        quaternion(get(c.t,1,true).pose.orientation,0,0,0,1);
    }
    pico_head_tracking_destroy(&c.t);
    printf("PICO_HEAD_TRACKING_PASS pairs=%u\n",pairs);
}
