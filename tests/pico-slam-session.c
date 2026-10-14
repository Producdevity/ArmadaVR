#include "pico-slam-session.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned created, destroyed, stopped, pose_created, pose_destroyed, frames, imus;
static int fault;
struct vit_tracker { vit_pose_data_t pending; bool available, started; unsigned next_camera; };
struct vit_pose { vit_pose_data_t data; };
static vit_result_t version(uint32_t *a,uint32_t *b,uint32_t *c) {*a=2;*b=0;*c=1;return VIT_SUCCESS;}
static vit_result_t create(const vit_config_t *c,vit_tracker_t **t)
{
    assert(c->cam_count==2 && c->imu_count==1 && !strcmp(c->file,"explicit-calibration.toml"));
    if(fault==1)return VIT_ERROR_ALLOCATION_FAILURE;
    *t=calloc(1,sizeof(**t));assert(*t);++created;return VIT_SUCCESS;
}
static void destroy(vit_tracker_t *t){assert(!t->started);++destroyed;free(t);}
static vit_result_t format(const vit_tracker_t *t,vit_image_format_t f,bool *supported)
{assert(t && f==VIT_IMAGE_FORMAT_L8);*supported=fault!=2;return VIT_SUCCESS;}
static vit_result_t start(vit_tracker_t *t){if(fault==3)return VIT_ERROR_INVALID_VALUE;t->started=true;return VIT_SUCCESS;}
static vit_result_t stop(vit_tracker_t *t){t->started=false;++stopped;return VIT_SUCCESS;}
static vit_result_t imu(vit_tracker_t *t,const vit_imu_sample_t *s)
{assert(t->started && s->ay==9.8f && s->wz==0.1f);++imus;return fault==4?VIT_ERROR_ALLOCATION_FAILURE:VIT_SUCCESS;}
static vit_result_t image(vit_tracker_t *t,const vit_img_sample_t *s)
{
    assert(t->started && s->cam_index==t->next_camera++%2 && s->format==VIT_IMAGE_FORMAT_L8);
    assert(s->size==6 && s->width==3 && s->height==2 && s->stride==3 && !memcmp(s->data,"abcdef",6));
    ++frames;if(fault==5)return VIT_ERROR_ALLOCATION_FAILURE;
    if(s->cam_index==1){assert(!t->available);t->pending=(vit_pose_data_t){.timestamp=s->timestamp,.ow=1,.px=0.2};t->available=true;}
    return VIT_SUCCESS;
}
static vit_result_t pop(vit_tracker_t *t,vit_pose_t **p)
{
    *p=NULL;if(fault==6)return VIT_ERROR_INVALID_VALUE;
    if(t->available){*p=malloc(sizeof(**p));assert(*p);(*p)->data=t->pending;t->available=false;++pose_created;}
    return VIT_SUCCESS;
}
static vit_result_t pose_data(const vit_pose_t *p,vit_pose_data_t *d)
{*d=p->data;if(fault==8)d->px=NAN;return fault==7?VIT_ERROR_INVALID_VALUE:VIT_SUCCESS;}
static void pose_destroy(vit_pose_t *p){++pose_destroyed;free(p);}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i));}
static void snapshot(uint8_t *p,unsigned id,int64_t time,uint64_t seq)
{
    memset(p,0,95);p[0]=id;put32(p+1,3);put32(p+5,2);put32(p+9,PICO_CAMERA_MONO8);
    put32(p+13,1);put64(p+25,time+52000);put64(p+33,time);put64(p+41,1000);
    put64(p+49,seq);put32(p+57,6);put32(p+65,1);memcpy(p+89,"abcdef",6);
}
static bool feed(struct pico_slam_session *s,uint64_t gen,int64_t t,uint64_t seq)
{
    uint8_t raw[95];
    struct xrt_imu_sample v={.timestamp_ns=t-1000,.accel_m_s2={0,9.8,0},.gyro_rad_secs={0,0,0.1}};
    if(!pico_slam_session_imu(s,gen,&v,0,t))return false;
    for(unsigned i=0;i<2;++i){snapshot(raw,i,t,seq);if(!pico_slam_session_camera(s,gen,raw,sizeof(raw),t+10000))return false;memset(raw,0xa5,sizeof(raw));}
    v.timestamp_ns=t+1000;return pico_slam_session_imu(s,gen,&v,0,t+10000);
}
static bool tracked(struct pico_slam_session *s,int64_t now)
{
    struct xrt_space_relation relation;int64_t sample;
    bool valid=pico_slam_tracking_get(pico_slam_session_tracking(s),now,now,&relation,&sample);
    if(!valid)assert(!sample && !relation.relation_flags && relation.pose.orientation.w==1);
    return valid;
}
int main(void)
{
    struct pico_slam_backend api={version,create,destroy,format,start,stop,imu,image,pop,pose_data,pose_destroy};
    struct pico_slam_input_config input={.count=2,.camera_ids={0,1},.width={3,3},.height={2,2},
        .max_age_ns=100000000,.max_gap_ns=10000000,.max_skew_ns=100000};
    struct pico_slam_tracking_config tracking={.openxr_from_slam.orientation.w=1,.imu_from_head.orientation.w=1,
        .max_age_ns=100000000,.max_gap_ns=10000000,.max_prediction_ns=50000000};
    struct pico_slam_session *s=NULL;
    assert(pico_slam_session_create(&input,&tracking,&api,"explicit-calibration.toml",&s));
    assert(!pico_slam_session_create(&input,&tracking,&api,"explicit-calibration.toml",&s));
    uint64_t gen=0;int64_t t=1000000000;size_t poses;
    for(unsigned i=0;i<1000;++i){
        ++gen;t+=100000000;assert(pico_slam_session_begin(s,gen,0,0,t+100000000,t-10000));
        assert(feed(s,gen,t,0));assert(pico_slam_session_poll(s,gen,t+10000,&poses)&&poses==1);assert(!tracked(s,t+10000));
        assert(feed(s,gen,t+2000000,1));assert(pico_slam_session_poll(s,gen,t+2010000,&poses)&&poses==1);assert(tracked(s,t+2010000));
        assert(!pico_slam_session_begin(s,gen,0,0,t+100000000,t));
        assert(!pico_slam_session_poll(s,gen-1,t+2010000,&poses));assert(tracked(s,t+2010000));
        if(i%2){pico_slam_session_stop(s);pico_slam_session_stop(s);}
        else assert(!pico_slam_session_poll(s,gen,t+12000001,&poses));
        assert(!tracked(s,t+12000001));
    }
    for(fault=1;fault<=8;++fault){
        ++gen;t+=100000000;
        bool begun=pico_slam_session_begin(s,gen,0,0,t+100000000,t-10000);
        assert(begun==(fault>3));
        if(begun){
            bool fed=feed(s,gen,t,0);assert(fed==(fault>=6));
            assert(!pico_slam_session_poll(s,gen,t+10000,&poses));
        }
        assert(!tracked(s,t+10000));
        pico_slam_session_stop(s);
    }
    fault=0;
    for(unsigned mode=0;mode<5;++mode){
        ++gen;t+=100000000;assert(pico_slam_session_begin(s,gen,0,0,t+5000000,t-10000));
        assert(feed(s,gen,t,0));assert(pico_slam_session_poll(s,gen,t+10000,&poses));
        if(mode==0) assert(!pico_slam_session_poll(s,gen,t+5000001,&poses));
        if(mode==1) assert(!pico_slam_session_poll(s,gen,t,&poses));
        if(mode==2) assert(!pico_slam_session_camera(s,gen,"bad",3,t+20000));
        if(mode==3){struct xrt_imu_sample v={.timestamp_ns=t+1000};assert(!pico_slam_session_imu(s,gen,&v,0,t+20000));}
        if(mode==4){uint8_t raw[95];snapshot(raw,0,t,0);assert(!pico_slam_session_camera(s,gen,raw,95,t+20000));}
        assert(!tracked(s,t+20000));
    }
    pico_slam_session_destroy(&s);pico_slam_session_destroy(&s);assert(!s);
    assert(created==destroyed && created==stopped && pose_created==pose_destroyed);
    printf("PICO_SLAM_SESSION_PASS generations=%llu trackers=%u images=%u imu=%u poses=%u\n",(unsigned long long)gen,created,frames,imus,pose_created);
    return 0;
}
