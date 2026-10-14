#include "pico-slam-tracking.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>

#define NS INT64_C(1000000000)
static unsigned checks;
static void near(float actual, float expected)
{
    assert(isfinite(actual) && fabsf(actual - expected) < 0.0001f);
    ++checks;
}
static struct pico_slam_tracking_config config(void)
{
    return (struct pico_slam_tracking_config){
        .openxr_from_slam={.orientation={0,0,0,1}}, .imu_from_head={.orientation={0,0,0,1}},
        .max_age_ns=100000000, .max_gap_ns=30000000, .max_prediction_ns=50000000};
}
static void unavailable(struct pico_slam_tracking *t, int64_t now, int64_t at)
{
    struct xrt_space_relation r; int64_t timestamp=42;
    assert(!pico_slam_tracking_get(t,now,at,&r,&timestamp));
    assert(!r.relation_flags && !timestamp && r.pose.orientation.w==1); ++checks;
}
static void *read_poses(void *arg)
{
    struct pico_slam_tracking *t=arg;
    for(unsigned i=0;i<20000;i++) {
        struct xrt_space_relation r; int64_t timestamp;
        if(pico_slam_tracking_get(t,2*NS,2*NS,&r,&timestamp)) {
            assert(timestamp>0 && r.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
            assert(isfinite(r.pose.position.x));
        }
    }
    return NULL;
}
int main(void)
{
    struct pico_slam_tracking *t=NULL;
    struct pico_slam_tracking_config c=config();
    c.openxr_from_slam.orientation.w=0;
    assert(!pico_slam_tracking_create(&c,&t) && !t);
    c=config(); c.max_prediction_ns=50000001;
    assert(!pico_slam_tracking_create(&c,&t) && !t);
    c=config(); assert(pico_slam_tracking_create(&c,&t));
    assert(!pico_slam_tracking_begin(t,0));
    assert(pico_slam_tracking_begin(t,1));
    struct xrt_pose p={.orientation={0,0,0,1}};
    assert(pico_slam_tracking_push(t,1,&p,NS,1000,NS+1000));
    unavailable(t,NS+1000,NS);
    p.position.x=.02f;
    assert(pico_slam_tracking_push(t,1,&p,NS+20000000,1000,NS+20001000));
    struct xrt_space_relation r; int64_t time;
    assert(pico_slam_tracking_get(t,NS+20001000,NS+30000000,&r,&time));
    near(r.pose.position.x,.03f); near(r.linear_velocity.x,1);
    assert(time==NS+20000000 && r.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
    assert(pico_slam_tracking_get(t,NS+20001000,NS+10000000,&r,&time)); near(r.pose.position.x,.01f);
    unavailable(t,NS+20001000,NS-1);
    unavailable(t,NS+20001000,NS+70000000);
    unavailable(t,NS+120000000,NS+20000000);
    unavailable(t,NS+20000000,NS+20000000);
    assert(!pico_slam_tracking_push(t,0,&p,NS+30000000,0,NS+30000000));
    assert(pico_slam_tracking_get(t,NS+20001000,NS+20000000,&r,&time));
    assert(!pico_slam_tracking_push(t,1,&p,NS+20000000,0,NS+20000000));
    unavailable(t,NS+20001000,NS+20000000);
    assert(!pico_slam_tracking_begin(t,1));
    for(unsigned mode=0;mode<5;mode++) {
        unsigned generation=mode+2;
        assert(pico_slam_tracking_begin(t,generation));
        p=(struct xrt_pose){.orientation={0,0,0,1}};
        assert(pico_slam_tracking_push(t,generation,&p,NS,0,NS));
        int64_t timestamp=NS+20000000, now=timestamp, uncertainty=0;
        if(mode==0)p.position.x=NAN;
        if(mode==1)p.orientation.w=2;
        if(mode==2)timestamp=NS+30000001, now=timestamp;
        if(mode==3)now=timestamp-1;
        if(mode==4)uncertainty=100000001;
        assert(!pico_slam_tracking_push(t,generation,&p,timestamp,uncertainty,now));
        unavailable(t,now,timestamp);
    }
    pico_slam_tracking_destroy(&t); assert(!t);
    c=config(); c.openxr_from_slam.orientation.z=c.openxr_from_slam.orientation.w=sqrtf(.5f);
    c.openxr_from_slam.position=(struct xrt_vec3){10,20,30};
    c.imu_from_head.position.x=.1f;
    assert(pico_slam_tracking_create(&c,&t) && pico_slam_tracking_begin(t,1));
    p=(struct xrt_pose){.orientation={0,0,0,1},.position={1,2,3}};
    assert(pico_slam_tracking_push(t,1,&p,NS,0,NS));
    assert(pico_slam_tracking_push(t,1,&p,NS+20000000,0,NS+20000000));
    assert(pico_slam_tracking_get(t,NS+20000000,NS+20000000,&r,&time));
    near(r.pose.position.x,8); near(r.pose.position.y,21.1f); near(r.pose.position.z,33);
    near(r.pose.orientation.z,sqrtf(.5f));
    p.orientation.z=p.orientation.w=sqrtf(.5f);
    assert(pico_slam_tracking_push(t,1,&p,NS+40000000,0,NS+40000000));
    assert(pico_slam_tracking_get(t,NS+40000000,NS+40000000,&r,&time));
    near(r.pose.position.x,7.9f); near(r.pose.position.y,21); near(r.pose.orientation.z,1);
    near(r.linear_velocity.x,-5); near(r.linear_velocity.y,-5);
    pico_slam_tracking_destroy(&t);
    c=config(); assert(pico_slam_tracking_create(&c,&t));
    pthread_t reader; assert(!pthread_create(&reader,NULL,read_poses,t));
    for(unsigned cycle=1;cycle<=1000;cycle++) {
        assert(pico_slam_tracking_begin(t,cycle));
        p=(struct xrt_pose){.orientation={0,0,0,1}};
        for(unsigned i=0;i<10;i++) {
            int64_t timestamp=2*NS-90000000+i*10000000;
            assert(pico_slam_tracking_push(t,cycle,&p,timestamp,0,timestamp));
        }
        assert(pico_slam_tracking_get(t,2*NS,2*NS,&r,&time));
        pico_slam_tracking_stop(t); unavailable(t,2*NS,2*NS);
    }
    assert(!pthread_join(reader,NULL));
    pico_slam_tracking_destroy(&t); pico_slam_tracking_destroy(&t);
    printf("PICO_SLAM_TRACKING_PASS checks=%u generations=1000 poses=10000 reader_queries=20000\n",checks);
}
