#include "quest-camera-capture.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, get_calls, returns, syncs, windows, centers, frames, invalidations;
static int get_error, return_error, fail_sync, fail_window, fail_center, fail_frame;
static struct quest_camera_frame incoming;
static uint64_t lower, upper;
static int64_t center;
static uint8_t raw[2][96];
static struct quest_camera_capture_config config;
#define CHECK(x) do { ++checks; assert(x); } while (0)
int quest_camera_buffer_get(int fd, int32_t session, int32_t device, struct quest_camera_frame *out)
{
    CHECK(fd==51 && session==17 && device==23); ++get_calls;
    if (get_error) return get_error;
    *out=incoming;
    return 0;
}
int quest_camera_buffer_return(int fd, int32_t session, int32_t device, uint64_t id)
{
    CHECK(fd==51 && session==17 && device==23 && id==incoming.id); ++returns;
    return return_error;
}
static bool sync_cpu(void *p, void *token, bool end)
{
    CHECK(p==&config && (token==raw[0] || token==raw[1]));
    ++syncs; CHECK(end==(syncs%2==0));
    return (int)syncs!=fail_sync;
}
static bool window(void *p, const struct quest_camera_frame *image,
                   const struct quest_camera_metadata *m, int64_t now, uint64_t *a, uint64_t *b)
{
    CHECK(p==&config && image->id==incoming.id && m->tag==176 && now>0); ++windows;
    *a=lower; *b=upper; return !fail_window;
}
static bool timestamp(void *p, const struct quest_camera_frame *image,
                       const struct quest_camera_metadata *m, const struct quest_camera_shutter *s,
                       int64_t now, int64_t *out)
{
    CHECK(p==&config && image->id==incoming.id && m->tag==176 && s->camera_mask==1 && now>0);
    ++centers; *out=center; return !fail_center;
}
static bool frame(void *p, uint64_t generation, const struct quest_camera_capture_frame *v, int64_t now)
{
    CHECK(p==&config && generation==42 && v->image.id==incoming.id && now>0); ++frames;
    CHECK(v->width==4 && v->height==2 && v->center_ns==center && v->metadata.tag==176);
    for (unsigned i=0;i<8;i++) CHECK(v->pixels[i]==i+1);
    return !fail_frame;
}
static void invalidate(void *p, uint64_t generation)
{
    CHECK(p==&config && generation==42); ++invalidations;
}
static const struct quest_camera_capture_ops ops={sync_cpu,window,timestamp,frame,invalidate};
static struct quest_camera_shutter shutter(uint64_t time)
{
    return (struct quest_camera_shutter){.timestamp_us=time,.camera_mask=1,.type=6,.tag=176};
}
static void reset(void)
{
    get_calls=returns=syncs=windows=centers=frames=invalidations=0;
    get_error=return_error=fail_sync=fail_window=fail_center=fail_frame=0;
    incoming=(struct quest_camera_frame){1,100,100,0}; lower=50; upper=60; center=100;
    memset(raw,0,sizeof(raw));
    const uint8_t row[]={0xc0,0x2f,0xea,0xd8,0x73,0x35,0x9f,0x7f,0x0b,0x62,0x04,0x88,
                          0x00,0x93,0x4f,0x39,0xd9,0x64,0x74,0xc2,0x1a,0x55,0x12,0xb0};
    for (unsigned i=0;i<2;i++) {
        memcpy(raw[i],row,sizeof(row));
        for (unsigned j=0;j<8;j++) raw[i][32+(j/4)*32+j%4]=(uint8_t)(j+1);
    }
    config=(struct quest_camera_capture_config){.fd=51,.session=17,.device=23,.generation=42,
        .camera_bit=1,.metadata_bits=8,.sensor=QUEST_CAMERA_OG01A1B,.metadata={{40,41274,38425},0},
        .layout={4,2,1,32,QUEST_CAMERA_MONO8,QUEST_CAMERA_CURVE_COPY},.metadata_size=24,
        .buffer_count=2,.timeout_ns=1000,.buffers={{1,raw[0],raw[0],96},{2,raw[1],raw[1],96}}};
}
static struct quest_camera_capture *create(void)
{
    struct quest_camera_capture *c=NULL;
    CHECK(quest_camera_capture_create(&config,&ops,&config,&c));
    CHECK(quest_camera_capture_poll(c,100)<0 && !get_calls);
    CHECK(quest_camera_capture_start(c,100));
    CHECK(!quest_camera_capture_start(c,100));
    CHECK(!quest_camera_capture_destroy(&c));
    return c;
}
static void finish(struct quest_camera_capture **c)
{
    unsigned old_gets=get_calls,old_returns=returns,old_syncs=syncs;
    quest_camera_capture_stop(*c); quest_camera_capture_stop(*c);
    CHECK(invalidations==1);
    CHECK(quest_camera_capture_poll(*c,101)<0);
    CHECK(!quest_camera_capture_destroy(c));
    CHECK(quest_camera_capture_quiesced(*c));
    CHECK(quest_camera_capture_destroy(c) && !*c);
    CHECK(quest_camera_capture_destroy(c));
    CHECK(get_calls==old_gets && returns==old_returns && syncs==old_syncs);
}
int main(void)
{
    reset();
    struct quest_camera_capture *c=NULL;
    CHECK(!quest_camera_capture_create(NULL,&ops,&config,&c));
    CHECK(!quest_camera_capture_create(&config,NULL,&config,&c));
    CHECK(!quest_camera_capture_destroy(NULL));
    CHECK(!quest_camera_capture_quiesced(NULL));
    struct quest_camera_capture_config valid=config;
#define BAD(expr) do { config=valid; expr; CHECK(!quest_camera_capture_create(&config,&ops,&config,&c) && !c); } while (0)
    BAD(config.fd=-1); BAD(config.device=0); BAD(config.session=0); BAD(config.generation=0);
    BAD(config.camera_bit=3); BAD(config.camera_bit=0); BAD(config.buffer_count=31); BAD(config.buffer_count=0);
    BAD(config.timeout_ns=0); BAD(config.timeout_ns=1000000001); BAD(config.sensor=0);
    BAD(config.metadata_bits=9); BAD(config.metadata.mcu_words[0]=0); BAD(config.metadata_size=33);
    BAD(config.layout.first_row=0); BAD(config.layout.width=4097); BAD(config.layout.height=0);
    BAD(config.layout.stride=3); BAD(config.layout.curve=QUEST_CAMERA_CURVE_POINT5);
    BAD(config.buffers[1].pixels=raw[0]+4); BAD(config.buffers[1].token=raw[0]); BAD(config.buffers[1].id=1);
    BAD(config.buffers[1].size=67); BAD(config.buffers[1].pixels=(void *)(UINTPTR_MAX-8));
    reset(); CHECK(quest_camera_capture_create(&config,&ops,&config,&c));
    CHECK(quest_camera_capture_destroy(&c) && !invalidations);
    for (unsigned order=0;order<2;order++) {
        reset(); c=create();
        for (unsigned n=0;n<256;n++) {
            int64_t now=100+n*10;
            incoming=(struct quest_camera_frame){1+n%2,now,now,n}; center=now;
            lower=50+n*10; upper=lower+9;
            struct quest_camera_shutter s=shutter(lower+5);
            if (order) CHECK(quest_camera_capture_shutter(c,42,&s,now));
            else { CHECK(quest_camera_capture_poll(c,now)==0); CHECK(quest_camera_capture_shutter(c,42,&s,now)); }
            CHECK(quest_camera_capture_poll(c,now)==1);
            CHECK(returns==n+1 && get_calls==n+1 && syncs==(n+1)*2 && frames==n+1);
            CHECK(quest_camera_capture_shutter(c,42,&s,now));
            CHECK(quest_camera_capture_shutter(c,41,NULL,-1));
        }
        finish(&c);
    }
    for (unsigned fault=0;fault<18;fault++) {
        reset(); c=create(); struct quest_camera_shutter s=shutter(55);
        CHECK(quest_camera_capture_shutter(c,42,&s,100));
        switch(fault) {
        case 0:get_error=-EFAULT;break; case 1:return_error=-EINTR;break;
        case 2:incoming.id=33;break; case 3:incoming.observed_monotonic_ns=0;break;
        case 4:incoming.observed_monotonic_ns=101;break; case 5:fail_sync=1;break;
        case 6:fail_sync=2;break; case 7:raw[0][5]=0;break; case 8:fail_window=1;break;
        case 9:lower=0;break; case 10:upper=49;break; case 11:fail_center=1;break;
        case 12:center=0;break; case 13:fail_frame=1;break;
        case 14:s.timestamp_us++;CHECK(quest_camera_capture_shutter(c,42,&s,100));break;
        case 15:finish(&c);reset();config.metadata_bits=10;c=create();CHECK(quest_camera_capture_shutter(c,42,&s,100));break;
        case 16:incoming.frame_number=-1;get_error=-EPROTO;break;
        case 17:incoming.qtimer_ns=0;get_error=-EPROTO;break;
        }
        CHECK(quest_camera_capture_poll(c,100)<0);
        CHECK(returns==(fault==1?1u:0u)); finish(&c);
    }
    reset(); c=create(); CHECK(quest_camera_capture_poll(c,100)==0);
    CHECK(quest_camera_capture_poll(c,1099)==0 && get_calls==1);
    CHECK(quest_camera_capture_poll(c,1100)<0 && returns==0); finish(&c);
    for (unsigned mode=0;mode<4;mode++) {
        reset(); c=create(); get_error=mode%2?-ETIMEDOUT:-ENOENT;
        CHECK(quest_camera_capture_poll(c,100)==0);
        CHECK(quest_camera_capture_poll(c,mode<2?99:1100)<0); finish(&c);
    }
    for (unsigned mode=0;mode<4;mode++) {
        reset(); c=create(); struct quest_camera_shutter s=shutter(55);
        CHECK(quest_camera_capture_shutter(c,42,&s,100)); CHECK(quest_camera_capture_poll(c,100)==1);
        incoming.frame_number=mode==0?0:1; incoming.qtimer_ns=mode==1?100:101;
        incoming.observed_monotonic_ns=101; lower=70; upper=80; center=mode==2?100:101;
        s=shutter(75); CHECK(quest_camera_capture_shutter(c,42,&s,101));
        CHECK(quest_camera_capture_poll(c,101)==(mode==3?1:mode==2?-ERANGE:-EPROTO));
        CHECK(frames==(mode==3?2u:1u)); finish(&c);
    }
    reset(); c=create(); incoming.observed_monotonic_ns=1;
    CHECK(quest_camera_capture_poll(c,1000)==0);
    CHECK(quest_camera_capture_poll(c,1001)==-ETIMEDOUT); finish(&c);
    for (unsigned kind=0;kind<3;kind++) {
        reset(); c=create(); struct quest_camera_shutter bad=shutter(55);
        if (kind==0) bad.timestamp_us=0;
        if (kind==1) bad.camera_mask=2;
        if (kind==2) bad.type=255;
        CHECK(!quest_camera_capture_shutter(c,42,&bad,100)); finish(&c);
    }
    reset(); c=create(); CHECK(!quest_camera_capture_quiesced(c));
    for (unsigned i=0;i<16;i++) { struct quest_camera_shutter s=shutter(50+i); CHECK(quest_camera_capture_shutter(c,42,&s,100)); }
    struct quest_camera_shutter s=shutter(99); CHECK(!quest_camera_capture_shutter(c,42,&s,100)); finish(&c);
    reset(); c=create(); s=shutter(55); CHECK(quest_camera_capture_shutter(c,42,&s,100));
    CHECK(!quest_camera_capture_shutter(c,42,&s,100)); finish(&c);
    printf("QUEST_CAMERA_CAPTURE_PASS cases=%u frames=512 ordering=2 hardware=0\n",checks);
}
