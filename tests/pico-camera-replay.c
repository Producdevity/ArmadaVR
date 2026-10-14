#define _GNU_SOURCE
#include "pico-camera-replay.h"
#include "pico-camera-stream.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

enum { CRM=1000031, SYNC=1000033 };
struct pico_camera_buffer { struct pico_camera_buffer_view view; bool flight, cpu; enum pico_camera_buffer_kind kind; };
static struct pico_camera_buffer *buffers[128], *images[2];
static bool live_mode;
static unsigned acquired_ife, linked, subscriptions, starts, stops, releases;
static struct pico_camera_event events[2][2];
static unsigned head[2], tail[2], sequence[2], frame_releases, next_fence, destroyed_fences;
static uint64_t request_ids[2];
static int32_t image_fences[2];
static uint32_t width, height, image_stride;
static void put(void *data, unsigned offset, uint64_t value, unsigned size)
{ for (unsigned i=0;i<size;++i) ((uint8_t *)data)[offset+i]=(uint8_t)(value>>(8*i)); }
static uint64_t get(const void *data, unsigned offset, unsigned size)
{ uint64_t value=0;for(unsigned i=0;i<size;++i)value|=(uint64_t)((const uint8_t *)data)[offset+i]<<(8*i);return value; }
int pico_camera_buffer_create(int crm,int ion,const struct pico_camera_buffer_config *c,struct pico_camera_buffer **out)
{
    assert(crm==CRM && (ion==1000032 || (live_mode && ion==-1)) && out && !*out && c->size && !(c->size%4096));
    unsigned i=0;while(i<128 && buffers[i])++i;assert(i<128);
    struct pico_camera_buffer *b=calloc(1,sizeof(*b));assert(b);
    b->view=(struct pico_camera_buffer_view){100+(int)i,200+(int)i,calloc(1,c->size),c->size};
    assert(b->view.data);b->kind=c->kind;buffers[i]=b;*out=b;return 0;
}
bool pico_camera_buffer_view(const struct pico_camera_buffer *b,struct pico_camera_buffer_view *v)
{ if(!b||!v)return false;*v=b->view;return true; }
int pico_camera_buffer_cpu_begin(struct pico_camera_buffer *b,enum ion_buffer_access access)
{ assert(b&&!b->cpu&&!b->flight&&(access==ION_BUFFER_READ||access==ION_BUFFER_WRITE));b->cpu=true;return 0; }
int pico_camera_buffer_cpu_end(struct pico_camera_buffer *b)
{ assert(b&&b->cpu&&!b->flight);b->cpu=false;return 0; }
int pico_camera_buffer_submit(struct pico_camera_buffer *b)
{ assert(b&&!b->cpu&&!b->flight);b->flight=true;return 0; }
int pico_camera_buffer_complete(struct pico_camera_buffer *b)
{ assert(b&&!b->cpu&&b->flight);b->flight=false;return 0; }
int pico_camera_buffer_release(struct pico_camera_buffer **out)
{
    if(!*out)return 0;
    struct pico_camera_buffer *b=*out;assert(!b->cpu&&!b->flight);
    if(b->kind==PICO_CAMERA_BUFFER_PIXELS)++frame_releases;
    memset(b->view.data,0xa5,b->view.size);free(b->view.data);
    buffers[b->view.memory_handle-100]=NULL;free(b);*out=NULL;return 0;
}
static bool rig_control(int fd,unsigned opcode,void *arg);
int ioctl(int fd,unsigned long request,...)
{
    va_list args;va_start(args,request);void *data=va_arg(args,void *);va_end(args);
    if(fd<1000030 || fd>1000040)return (int)syscall(SYS_ioctl,fd,request,data);
    if(request==0x80885659UL) {
        assert(fd==CRM||fd==SYNC);unsigned source=fd==CRM;
        if(head[source]==tail[source]) { errno=EAGAIN;return -1; }
        struct pico_camera_event *e=&events[source][head[source]++];
        put(data,0,e->type,4);memcpy((uint8_t *)data+8,e->data,64);put(data,76,e->sequence,4);put(data,96,e->id,4);return 0;
    }
    if(request==0x4020565aUL || request==0x4020565bUL) {
        assert(live_mode && (fd==CRM || fd==SYNC) && get(data,0,4)==0x08000000 && get(data,4,4)<=1);
        if(request==0x4020565aUL)++subscriptions;else {assert(subscriptions);--subscriptions;}return 0;
    }
    assert(request==0xc01856c0UL);
    unsigned opcode=get(data,0,4);void *arg=(void *)(uintptr_t)get(data,16,8);
    if(live_mode && rig_control(fd,opcode,arg))return 0;
    if(fd==SYNC) {
        if(!opcode)put(arg,64,++next_fence,4);
        else if(opcode==4)assert(get(arg,8,8)==1);
        else { assert(opcode==1);++destroyed_fences; }
    } else if(fd==CRM)assert(opcode==0x10f);
    else {
        assert(fd>=1000034 && opcode==0x105);unsigned camera=fd==1000040?(unsigned)(get(arg,4,4)-42)/2:(unsigned)(fd-1000034)/2;assert(camera<2);
        struct pico_camera_buffer *packet=buffers[get(arg,16,8)-100];assert(packet&&packet->flight);
        if(fd%2 || fd==1000040) {
            request_ids[camera]=get(packet->view.data,8,8);
            images[camera]=buffers[get(packet->view.data,80,4)-100];
            image_fences[camera]=(int32_t)get(packet->view.data,272,4);
            assert(images[camera]&&images[camera]->flight);
        }
    }
    return 0;
}
void pico_camera_replay_config(struct pico_camera_transport_config *c,uint32_t w,uint32_t h,const uint8_t ids[2])
{
    width=w;height=h;image_stride=((w+15)/16+1)*16;
    memset(c,0,sizeof(*c));c->timeout_ns=100000000;
    for(unsigned i=0;i<2;++i)c->cameras[i]=(struct pico_camera_request_config){
        .crm_fd=CRM,.sync_fd=SYNC,.ion_fd=1000032,.sensor_fd=1000034+(int)i*2,.ife_fd=1000035+(int)i*2,
        .session=77,.link=100+(int)i,.sensor=41+(int)i*2,.ife=42+(int)i*2,
        .device_mmu=71,.command_mmu=72,.command_mapping=PICO_CAMERA_MEMORY_SHARED,.ion_heap=25,
        .width=w,.height=h,.stride=image_stride,.camera_id=ids[i],.generation=1,.request_id=1,.synchronized=true};
    sequence[0]=sequence[1]=UINT32_MAX-3;
}
void pico_camera_replay_feed(unsigned camera,const uint8_t *pixels,size_t stride,int64_t qtimer_ns)
{
    assert(camera<2 && images[camera] && images[camera]->flight && stride>=width);
    if(head[0]==tail[0] && head[1]==tail[1])head[0]=head[1]=tail[0]=tail[1]=0;
    struct pico_camera_buffer *b=images[camera];memset(b->view.data,0xee,b->view.size);
    for(unsigned row=0;row<height;++row)memcpy((uint8_t *)b->view.data+row*image_stride,pixels+row*stride,width);
    assert(tail[0]<2&&tail[1]<2);
    struct pico_camera_event *f=&events[0][tail[0]++],*s=&events[1][tail[1]++];
    *f=(struct pico_camera_event){.type=0x08000000,.sequence=sequence[0]++};
    *s=(struct pico_camera_event){.type=0x08000000,.sequence=sequence[1]++};
    put(f->data,0,image_fences[camera],4);put(f->data,4,2,4);put(f->data,8,1,8);put(f->data,16,request_ids[camera],8);
    put(s->data,0,77,4);put(s->data,8,request_ids[camera],8);put(s->data,16,request_ids[camera]-1,8);
    put(s->data,24,qtimer_ns,8);put(s->data,32,100+camera,4);
}
unsigned pico_camera_replay_finish(void)
{
    for(unsigned i=0;i<128;++i)assert(!buffers[i]);
    if(live_mode)assert(!subscriptions && starts==8 && stops==8 && releases==11);
    assert(next_fence==destroyed_fences && next_fence==frame_releases && head[0]==tail[0] && head[1]==tail[1]);
    return frame_releases;
}

static bool rig_control(int fd,unsigned opcode,void *arg)
{
    if(opcode==0x101) {
        if(fd==1000034 || fd==1000036) {unsigned i=(fd-1000034)/2;put(arg,0,2+i,4);put(arg,36,3+i,4);}
        else if(fd==1000038 || fd==1000039){put(arg,0,3+fd-1000038,4);put(arg,4,0x121,4);}
        else {
            assert(fd==1000040);void *caps=(void *)(uintptr_t)get(arg,8,8);
            put(caps,0,71,4);put(caps,8,72,4);put(caps,16,2,4);
            put(caps,24,1,4);put(caps,28,480,4);put(caps,48,1,4);put(caps,52,480,4);
        }
    } else if(opcode==0x10b){assert(fd==CRM);put(arg,0,77,4);}
    else if(opcode==0x102) {
        assert(get(arg,0,4)==77);
        unsigned handle=fd==1000040?42+acquired_ife++*2:fd>=1000038?(unsigned)(45+fd-1000038):(unsigned)(41+fd-1000034);
        put(arg,4,handle,4);
    } else if(opcode==0x151){assert(fd==1000040);put(arg,32,1U<<((get(arg,12,4)-42)/2),4);put(arg,52,8,4);put(arg,112,1,4);}
    else if(opcode==0x10d){assert(fd==CRM);put(arg,264,100+linked++,4);}
    else if(opcode==0x111){assert(fd==CRM && linked==2);}
    else if(opcode==0x116){if(get(arg,0,4)==0)++starts;else ++stops;}
    else if(opcode==0x103){++starts;}
    else if(opcode==0x104){++stops;}
    else if(opcode==0x106 || opcode==0x152 || opcode==0x10e || opcode==0x10c){++releases;}
    else if(opcode==0x105) {
        struct pico_camera_buffer *packet=buffers[get(arg,16,8)-100];assert(packet&&packet->flight);
        unsigned type=get(packet->view.data,0,4);
        if(type==0xf000001 || type==0x100007f)return false;
        assert(type==0x1000000 || type==0x1000002 || type==0x1000004 || type==0x1000005 || type==0xf000000 || type==0x5000000);
    } else return false;
    return true;
}
int fcntl(int fd,int op,...)
{
    if(fd==CRM || fd==SYNC){assert(op==F_GETFL);return O_NONBLOCK;}
    va_list args;va_start(args,op);long arg=0;
    if(op==F_DUPFD || op==F_DUPFD_CLOEXEC || op==F_SETFD || op==F_SETFL || op==F_SETOWN)arg=va_arg(args,int);
    else if(op!=F_GETFL && op!=F_GETFD && op!=F_GETOWN)arg=(long)va_arg(args,void *);
    va_end(args);return (int)syscall(SYS_fcntl,fd,op,arg);
}
struct ssc_imu_session { struct ssc_imu_session_config config; bool stopped; };
static struct ssc_imu_session *sensor;
static _Thread_local int64_t sample_now;
int clock_gettime(clockid_t id,struct timespec *ts)
{
    if(id==CLOCK_MONOTONIC && sample_now){ts->tv_sec=sample_now/1000000000;ts->tv_nsec=sample_now%1000000000;return 0;}
    return (int)syscall(SYS_clock_gettime,id,ts);
}
bool ssc_imu_session_create(const struct ssc_imu_session_config *c,struct ssc_imu_session **out)
{
    assert(live_mode && !sensor && !*out && subscriptions==3 && !starts);
    sensor=calloc(1,sizeof(*sensor));assert(sensor);sensor->config=*c;*out=sensor;return true;
}
void ssc_imu_session_stop(struct ssc_imu_session *s){assert(s==sensor);s->stopped=true;}
bool ssc_imu_session_destroy(struct ssc_imu_session **s)
{ if(!s || !*s)return true;assert(*s==sensor && sensor->stopped);free(sensor);sensor=NULL;*s=NULL;return true; }
void ssc_imu_session_get_status(struct ssc_imu_session *s,struct ssc_imu_session_status *status)
{ assert(s==sensor);memset(status,0,sizeof(*status));status->stopping=s->stopped; }
static bool calibrate(void *data,struct ssc_clock *clock)
{ (void)data;memset(clock,0,sizeof(*clock));clock->initialized=true;return true; }
void pico_camera_replay_imu(const struct xrt_imu_sample *s,uint64_t ticks,int64_t now)
{
    assert(sensor && !sensor->stopped);sample_now=now;
    for(unsigned n=0;n<2;n++) {
        unsigned i=(ticks+n)%2;
        struct ssc_bias_sample b={.raw={.sensor=i,.generation=1,.ticks=ticks,.time={s->timestamp_ns,0}},.accuracy=3};
        struct xrt_vec3_f64 v=i?s->gyro_rad_secs:s->accel_m_s2;
        b.corrected[0]=v.x;b.corrected[1]=v.y;b.corrected[2]=v.z;
        sensor->config.bias.report(sensor->config.data,&b);
    }
    sample_now=0;
}
void pico_camera_replay_live_config(struct pico_slam_live_config *c,uint32_t w,uint32_t h,const uint8_t ids[2])
{
    struct pico_camera_transport_config tc;pico_camera_replay_config(&tc,w,h,ids);
    live_mode=true;c->sync_fd=SYNC;c->ion_fd=1000032;c->ion_heap=25;
    c->stride[0]=c->stride[1]=image_stride;c->queue_depth=1;c->first_request_id=1;
    c->frame_period_ns=20000000;c->capture_timeout_ns=100000000;
    c->tracking.sensors.calibrate=calibrate;
    c->cameras.session.crm_fd=CRM;c->cameras.session.ife_fd=1000040;
    c->cameras.command_mapping=PICO_CAMERA_MEMORY_SHARED;
    static struct pico_camera_sensor_write writes[2]={{0x100,1,0},{0x100,2,0}};
    for(unsigned i=0;i<2;i++) {
        c->cameras.session.cameras[i].sensor_fd=1000034+i*2;
        c->cameras.session.cameras[i].phy_fd=1000038+i;
        c->cameras.session.cameras[i].sensor_slot=2+i;c->cameras.session.cameras[i].phy_version=0x121;
        c->cameras.session.cameras[i].input=(struct pico_camera_ife_config){.phy=i+3,.lanes=2,.lane_map=0x3210,
            .virtual_channel=0,.data_type=0x2a,.width=w,.height=h,.bits=8,.output=PICO_CAMERA_RAW_MIPI};
        struct pico_camera_sensor_table table={{2,1,1,false,0xe2+i*2},writes+i,1};
        c->cameras.cameras[i].initialize=c->cameras.cameras[i].mode=c->cameras.cameras[i].stream_on=c->cameras.cameras[i].stream_off=table;
        c->cameras.cameras[i].phy=(struct pico_camera_csiphy_config){0,0,2,0x3210,14,384000000,0};
        c->cameras.cameras[i].clocks=(struct pico_camera_ife_clocks){400000000,0,200000000,false};
        c->cameras.cameras[i].bandwidth=(struct pico_camera_ife_bandwidth){640000000,800000000,1000000000};
    }
}
