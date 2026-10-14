#include "pico-slam-input.h"
#include <xrt/xrt_frame.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, images, imus, order;
#undef assert
#define assert(x) do { ++checks; if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while(0)
#include <stdlib.h>
struct receiver { struct xrt_frame_sink sink; struct xrt_frame *retained; unsigned index; };
static void frame(struct xrt_frame_sink *sink, struct xrt_frame *f)
{
    struct receiver *r=(struct receiver *)sink;
    assert(r->index==order++); assert(f->size==6 && f->stride==3 && f->width==3 && f->height==2);
    assert(f->format==XRT_FORMAT_L8 && f->stereo_format==XRT_STEREO_FORMAT_NONE);
    assert(!memcmp(f->data,"abcdef",6)); assert(f->reference.count==1);
    xrt_frame_reference(&r->retained,f); ++images;
}
static void imu(struct xrt_imu_sink *sink, struct xrt_imu_sample *sample)
{
    (void)sink; assert(sample->accel_m_s2.y==9.8 && sample->gyro_rad_secs.z==0.1); ++imus;
    sample->timestamp_ns=-1;
}
static void put32(uint8_t *p, uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p, uint64_t n) {for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i));}
static void snapshot(uint8_t *p,unsigned id,int64_t time,uint64_t sequence)
{
    memset(p,0,95);p[0]=(uint8_t)id;put32(p+1,3);put32(p+5,2);put32(p+9,PICO_CAMERA_MONO8);
    put32(p+0xd,10);put64(p+0x19,time+10000);put64(p+0x21,time);put64(p+0x29,1000);
    put64(p+0x31,sequence);put32(p+0x39,6);put32(p+0x41,1);memcpy(p+89,"abcdef",6);
}
static struct xrt_imu_sample sample(int64_t time)
{
    return (struct xrt_imu_sample){.timestamp_ns=time,.accel_m_s2={0,9.8,0},.gyro_rad_secs={0,0,0.1}};
}
int main(void)
{
    struct receiver receivers[4]={0};struct xrt_imu_sink isink={.push_imu=imu};
    struct xrt_slam_sinks sinks={.cam_count=4,.imu=&isink};
    for(unsigned i=0;i<4;++i){receivers[i].index=i;receivers[i].sink.push_frame=frame;sinks.cams[i]=&receivers[i].sink;}
    struct pico_slam_input_config c={.count=4,.camera_ids={3,2,1,0},.width={3,3,3,3},.height={2,2,2,2},
        .max_age_ns=10000000,.max_gap_ns=5000000,.max_skew_ns=100000};
    struct pico_slam_input *s=NULL;assert(pico_slam_input_create(&c,&sinks,&s));
    assert(!pico_slam_input_create(&c,&sinks,&s));
    int64_t t=1000000000,now=t+1000000;uint8_t raw[95];
    struct xrt_imu_sample value=sample(t-1000);
    assert(!pico_slam_input_imu(s,1,&value,0,now));
    assert(!pico_slam_input_begin(s,0,0,0,now+1000000));
    assert(!pico_slam_input_begin(s,1,0,50001,now+1000000));
    assert(pico_slam_input_begin(s,1,-100,0,now+1000000));
    assert(!pico_slam_input_begin(s,1,0,0,now+1000000));
    assert(pico_slam_input_imu(s,1,&value,0,now));assert(value.timestamp_ns==t-1000);
    for(unsigned i=0;i<4;++i) {
        snapshot(raw,i,t+100+i*1000,5);
        assert(pico_slam_input_camera(s,1,raw,95,now));memset(raw,0x55,95);
        assert(images==0);
    }
    value=sample(t+3000);assert(pico_slam_input_imu(s,1,&value,0,now));assert(images==4);
    for(unsigned i=0;i<4;++i) {
        struct xrt_frame *f=receivers[i].retained;
        assert(f->source_id==3-i && f->source_sequence==5 && f->source_timestamp==t+100+(3-i)*1000);
        assert(f->timestamp==t && f->reference.count==1);
    }
    assert(!pico_slam_input_imu(s,1,&value,0,now));
    value=sample(t+4000);value.gyro_rad_secs.x=NAN;assert(!pico_slam_input_imu(s,1,&value,0,now));
    value=sample(now+1);assert(!pico_slam_input_imu(s,1,&value,0,now));
    snapshot(raw,0,t+100,5);assert(!pico_slam_input_camera(s,1,raw,95,now));
    snapshot(raw,99,t+100,6);assert(!pico_slam_input_camera(s,1,raw,95,now));
    snapshot(raw,0,t+100,6);put32(raw+0x41,0);assert(!pico_slam_input_camera(s,1,raw,95,now));
    assert(!pico_slam_input_camera(s,2,raw,95,now));
    value=sample(t+6000000);assert(!pico_slam_input_imu(s,1,&value,0,t+6000000));
    value=sample(t+4000);assert(!pico_slam_input_imu(s,1,&value,0,now));
    for(uint64_t gen=2;gen<=1001;++gen) {
        order=0;unsigned before=images;
        assert(pico_slam_input_begin(s,gen,0,1,now+10000000));
        value=sample(t-1000);assert(pico_slam_input_imu(s,gen,&value,1,now));
        snapshot(raw,0,t,0);assert(pico_slam_input_camera(s,gen,raw,95,now));
        for(unsigned i=1;i<4;++i){snapshot(raw,i,t+100000,0);assert(pico_slam_input_camera(s,gen,raw,95,now));}
        value=sample(t+200000);assert(pico_slam_input_imu(s,gen,&value,1,now));
        assert(images==before); /* Clock uncertainty makes the exact raw bound insufficient. */
        snapshot(raw,0,t+100000,1);assert(pico_slam_input_camera(s,gen,raw,95,now));
        assert(images==before+4);
        for(unsigned i=0;i<4;++i)assert(!memcmp(receivers[i].retained->data,"abcdef",6));
        snapshot(raw,0,t+200000,2);assert(pico_slam_input_camera(s,gen,raw,95,now));
        pico_slam_input_stop(s);assert(!pico_slam_input_camera(s,gen,raw,95,now));
        assert(!pico_slam_input_begin(s,gen,0,0,now+10000000));
    }
    assert(pico_slam_input_begin(s,1002,INT64_MAX,0,now+10000000));
    snapshot(raw,0,t,0);assert(!pico_slam_input_camera(s,1002,raw,95,now));
    assert(pico_slam_input_begin(s,1003,INT64_MIN,0,now+10000000));
    assert(!pico_slam_input_camera(s,1003,raw,95,now));
    assert(pico_slam_input_begin(s,1004,0,0,now-1));
    value=sample(t);assert(!pico_slam_input_imu(s,1004,&value,0,now));
    pico_slam_input_destroy(&s);assert(!s);
    for(unsigned i=0;i<4;++i){assert(!memcmp(receivers[i].retained->data,"abcdef",6));xrt_frame_reference(&receivers[i].retained,NULL);}
    assert(pico_slam_input_create(&c,&sinks,&s));
    assert(pico_slam_input_begin(s,1,0,0,now+100000000));
    value=sample(t);assert(pico_slam_input_imu(s,1,&value,0,now));
    value=sample(t+c.max_gap_ns+1);assert(!pico_slam_input_imu(s,1,&value,0,value.timestamp_ns+1));
    assert(pico_slam_input_begin(s,2,0,0,now+100000000));
    snapshot(raw,0,t,1);assert(pico_slam_input_camera(s,2,raw,95,now));
    snapshot(raw,0,t+c.max_gap_ns+1,2);assert(!pico_slam_input_camera(s,2,raw,95,t+c.max_gap_ns+2));
    assert(pico_slam_input_begin(s,3,0,0,now+100000000));
    unsigned discarded=images;
    value=sample(t+10);assert(pico_slam_input_imu(s,3,&value,0,now));
    for(unsigned i=0;i<4;++i){snapshot(raw,i,t,0);assert(pico_slam_input_camera(s,3,raw,95,now));}
    assert(images==discarded); /* No IMU history before the images. */
    assert(pico_slam_input_begin(s,4,0,0,now+100000000));
    value=sample(t-100);assert(pico_slam_input_imu(s,4,&value,0,now));
    for(unsigned i=0;i<3;++i){snapshot(raw,i,t,0);assert(pico_slam_input_camera(s,4,raw,95,now));}
    snapshot(raw,3,t,0);assert(!pico_slam_input_camera(s,4,raw,95,t+c.max_age_ns+1));
    value=sample(t+1);assert(!pico_slam_input_imu(s,4,&value,0,now)); /* Arrival clock reversed. */
    assert(images==discarded);
    assert(pico_slam_input_begin(s,5,0,50000,now+100000000));
    snapshot(raw,0,t,0);assert(!pico_slam_input_camera(s,5,raw,95,t+49999));
    assert(!pico_slam_input_camera(s,5,raw,95,t+10000000-49999));
    pico_slam_input_destroy(&s);
    c.camera_ids[0]=c.camera_ids[1];assert(!pico_slam_input_create(&c,&sinks,&s));
    c.camera_ids[0]=3;c.count=3;assert(!pico_slam_input_create(&c,&sinks,&s));
    c.count=2;sinks.cam_count=2;
    assert(pico_slam_input_create(&c,&sinks,&s));
    order=0;unsigned before=images;
    assert(pico_slam_input_begin(s,1,0,0,now+10000000));
    value=sample(t-1000);assert(pico_slam_input_imu(s,1,&value,0,now));
    for(unsigned i=0;i<2;++i){snapshot(raw,c.camera_ids[i],t,0);assert(pico_slam_input_camera(s,1,raw,95,now));}
    value=sample(t+1000);assert(pico_slam_input_imu(s,1,&value,0,now));assert(images==before+2);
    pico_slam_input_destroy(&s);
    for(unsigned i=0;i<2;++i)xrt_frame_reference(&receivers[i].retained,NULL);
    for (unsigned cameras=2; cameras<=4; cameras+=2) {
        c.count=cameras; sinks.cam_count=(int)cameras;
        assert(pico_slam_input_create(&c,&sinks,&s));
        for (uint64_t gen=1; gen<=1000; ++gen) {
            order=0; before=images;
            assert(pico_slam_input_begin(s,gen,-100,0,now+10000000));
            value=sample(t-1000); assert(pico_slam_input_imu(s,gen,&value,0,now));
            uint8_t padded[33];
            struct pico_slam_mono8_frame view={c.camera_ids[0],3,2,16,19,padded+1,t+100,5};
            assert(!pico_slam_input_mono8(s,gen,NULL,now));
#define BAD_VIEW(field, v) do { struct pico_slam_mono8_frame bad=view; bad.field=v; assert(!pico_slam_input_mono8(s,gen,&bad,now)); } while(0)
            BAD_VIEW(pixels,NULL); BAD_VIEW(width,0); BAD_VIEW(height,4097); BAD_VIEW(stride,2);
            BAD_VIEW(stride,SIZE_MAX); BAD_VIEW(size,18); BAD_VIEW(center_ns,0); BAD_VIEW(center_ns,INT64_MAX);
            BAD_VIEW(camera_id,99);
            assert(!pico_slam_input_mono8(s,gen+1,&view,now));
            for (unsigned i=0; i<cameras; ++i) {
                memset(padded,0xee,sizeof(padded));memcpy(padded+1,"abc",3);memcpy(padded+17,"def",3);
                view.camera_id=c.camera_ids[i];
                assert(pico_slam_input_mono8(s,gen,&view,now)); memset(padded,0xa5,sizeof(padded));
                assert(!pico_slam_input_mono8(s,gen,&view,now));
            }
            assert(images==before);
            value=sample(t+1000);assert(pico_slam_input_imu(s,gen,&value,0,now));
            assert(images==before+cameras);
            for (unsigned i=0; i<cameras; ++i) {
                assert(!memcmp(receivers[i].retained->data,"abcdef",6));
                assert(receivers[i].retained->timestamp==t&&receivers[i].retained->source_timestamp==t+100);
            }
            pico_slam_input_stop(s);assert(!pico_slam_input_mono8(s,gen,&view,now));
        }
        pico_slam_input_destroy(&s);
        for(unsigned i=0;i<cameras;++i)xrt_frame_reference(&receivers[i].retained,NULL);
    }
    printf("PICO_SLAM_INPUT_PASS checks=%u images=%u imu=%u generations=3010\n",checks,images,imus);
}
