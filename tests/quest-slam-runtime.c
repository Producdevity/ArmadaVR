#define _POSIX_C_SOURCE 200809L
#include "quest-slam-runtime.h"
#include <errno.h>
#include <time.h>
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

static int64_t clock_ns=1000000000, pending_timestamp;
static int sensor_error, clock_error;
static unsigned sensor_opens,sensor_closes;
struct quest_imu_stream {struct quest_imu_stream_config config;bool stopped;};
int quest_test_clock(clockid_t c,struct timespec *t)
{assert(c==CLOCK_MONOTONIC);if(clock_error)return -1;t->tv_sec=clock_ns/1000000000;t->tv_nsec=clock_ns%1000000000;return 0;}
bool quest_imu_stream_open(const struct quest_imu_stream_config *c,struct quest_imu_stream **out)
{assert(c && !*out && c->imu.timing && c->imu.timing->max_gap_ns==2500000);++sensor_opens;if(sensor_error)return false;*out=calloc(1,sizeof(**out));assert(*out);(*out)->config=*c;return true;}
int quest_imu_stream_poll(struct quest_imu_stream *s)
{
 assert(s);if(s->stopped)return -ECANCELED;
 if(sensor_error){s->config.invalidated(s->config.userdata,s->config.generation,sensor_error);return sensor_error;}
 if(!pending_timestamp)return 0;
 struct quest_imu_sample sample={.timestamp_ns=pending_timestamp,.sample={.acceleration_m_s2={0,9.8f,0},.angular_velocity_rad_s={0,0,.1f}}};
 pending_timestamp=0;
 if(!s->config.sample(s->config.userdata,s->config.generation,&sample)){s->config.invalidated(s->config.userdata,s->config.generation,-EPIPE);return -EPIPE;}
 return 1;
}
void quest_imu_stream_stop(struct quest_imu_stream *s)
{if(s && !s->stopped){s->stopped=true;++sensor_closes;s->config.invalidated(s->config.userdata,s->config.generation,-ECANCELED);}}
void quest_imu_stream_destroy(struct quest_imu_stream **s)
{if(s && *s){quest_imu_stream_stop(*s);free(*s);*s=NULL;}}
static bool feed(struct quest_slam_runtime *s,int64_t t,uint64_t sequence)
{
 size_t poses;clock_ns=t+10000;pending_timestamp=t-1000;if(quest_slam_runtime_poll(s,&poses)!=1)return false;
 for(unsigned i=0;i<2;++i){struct pico_slam_mono8_frame f={(uint8_t)i,3,2,3,6,(const uint8_t *)"abcdef",t,sequence};if(!quest_slam_runtime_camera(s,42,&f))return false;}
 pending_timestamp=t+1000;if(quest_slam_runtime_poll(s,&poses)!=1)return false;
 return poses==1;
}
static bool tracked(struct quest_slam_runtime *s)
{
 struct xrt_space_relation relation;int64_t sample;
 bool valid=pico_slam_tracking_get(quest_slam_runtime_tracking(s),clock_ns,clock_ns,&relation,&sample);
 if(!valid)assert(!sample && !relation.relation_flags && relation.pose.orientation.w==1);
 return valid;
}
static struct quest_slam_runtime_config configuration(void)
{
 static const struct imu_temperature_offset offset[]={{25,{0,0,0}}};
 static const struct quest_imu_timing timing={0,0,2500000};
 return (struct quest_slam_runtime_config){.sensors={.generation=42,.max_gap_ns=2500000,
 .imu={.format=2,.parameters={16.384f,4096,25,128,false},.max_age_ns=5000000,
 .frame=IMU_FRAME_ACCELEROMETER,.accelerometer={1,0,0,0,1,0,0,0,1},.gyroscope={1,0,0,0,1,0,0,0,1},.device_from_imu={1,0,0,0,1,0,0,0,1},
 .accelerometer_offsets=offset,.gyroscope_offsets=offset,.accelerometer_offset_count=1,.gyroscope_offset_count=1,.timing=&timing}},
 .input={.count=2,.camera_ids={0,1},.width={3,3},.height={2,2},.max_age_ns=100000000,.max_gap_ns=10000000,.max_skew_ns=100000},
 .tracking={.openxr_from_slam.orientation.w=1,.imu_from_head.orientation.w=1,.max_age_ns=100000000,.max_gap_ns=10000000,.max_prediction_ns=50000000},
 .backend={version,create,destroy,format,start,stop,imu,image,pop,pose_data,pose_destroy},.backend_config="explicit-calibration.toml",.clock_expires_ns=clock_ns+100000000};
}
int main(void)
{
 unsigned runs=0;
 for(unsigned cycle=0;cycle<200;++cycle){
  unsigned mode=cycle%13;
  fault=clock_error=sensor_error=0;clock_ns+=1000000000;pending_timestamp=0;
  struct quest_slam_runtime_config c=configuration();struct quest_slam_runtime *s=NULL;
  struct quest_imu_timing timing=*c.sensors.imu.timing;c.sensors.imu.timing=&timing;
  unsigned opened=sensor_opens;assert(quest_slam_runtime_prepare(&c,&s));assert(sensor_opens==opened);timing.max_gap_ns=0;
  size_t poses=999;assert(quest_slam_runtime_poll(s,&poses)==-EAGAIN && !poses);
  assert(quest_slam_runtime_start(s));assert(!quest_slam_runtime_start(s));
  assert(quest_slam_runtime_poll(s,&poses)==0 && !poses);assert(!tracked(s));
  int64_t t=clock_ns+10000;
  assert(feed(s,t,0));assert(!tracked(s));assert(feed(s,t+2000000,1));assert(tracked(s));
  assert(!quest_slam_runtime_camera(s,41,NULL) && tracked(s));
  if(mode==0)quest_slam_runtime_stop(s);
  if(mode==1)sensor_error=-ENODEV;
  if(mode==2)clock_error=1;
  if(mode==3)--clock_ns;
  if(mode==4)clock_ns=c.clock_expires_ns;
  if(mode==5)clock_ns+=10000001;
  if(mode==6){assert(!quest_slam_runtime_camera(s,42,NULL));}
  if(mode>=7 && mode<12){fault=mode-3;assert(!feed(s,t+4000000,2));}
  if(mode==12)pending_timestamp=clock_ns+1;
  assert(quest_slam_runtime_poll(s,&poses)<0 && !poses);assert(!tracked(s));
  assert(!quest_slam_runtime_start(s));quest_slam_runtime_stop(s);quest_slam_runtime_destroy(&s);quest_slam_runtime_destroy(&s);assert(!s);++runs;
 }
 fault=clock_error=sensor_error=0;clock_ns+=1000000000;
 for(unsigned mode=0;mode<7;++mode){struct quest_slam_runtime_config c=configuration();struct quest_slam_runtime *s=NULL;
  if(mode==0)c.sensors.imu.timing=NULL;
  if(mode==1)c.imu_uncertainty_ns=-1;
  if(mode==2)c.sensors.generation=0;
  if(mode==3)c.sensors.max_gap_ns=0;
  if(mode==4)c.sensors.userdata=&c;
  if(mode==5)c.camera_uncertainty_ns=1000000;
  if(mode==6)c.clock_expires_ns=0;
  unsigned before=sensor_opens;assert(!quest_slam_runtime_prepare(&c,&s) && !s && sensor_opens==before);++runs;}
 for(unsigned mode=0;mode<5;++mode){
  fault=clock_error=sensor_error=0;clock_ns+=1000000000;pending_timestamp=0;
  struct quest_slam_runtime_config c=configuration();struct quest_slam_runtime *s=NULL;
  assert(quest_slam_runtime_prepare(&c,&s));size_t poses;
  if(mode==4){quest_slam_runtime_stop(s);assert(!quest_slam_runtime_start(s));}
  else {
   if(mode==3)sensor_error=-EACCES;else fault=mode+1;
   bool started=quest_slam_runtime_start(s);assert(started==(mode!=3));
   pending_timestamp=clock_ns-1000;
   assert(quest_slam_runtime_poll(s,&poses)<0 && !poses && !tracked(s));
  }
  assert(!quest_slam_runtime_start(s));quest_slam_runtime_destroy(&s);++runs;
 }
 assert(sensor_opens==sensor_closes+1 && created==destroyed && created==stopped && pose_created==pose_destroyed);
 printf("QUEST_SLAM_RUNTIME_PASS runs=%u sensors=%u trackers=%u images=%u imu=%u poses=%u\n",runs,sensor_opens,created,frames,imus,pose_created);
}
