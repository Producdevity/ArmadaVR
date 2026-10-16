extern "C" {
#include "quest-camera-capture.h"
#include "quest-camera-stream.h"
#include "quest-slam-runtime.h"
void quest_replay_shutter(int64_t timestamp);
}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cerrno>
#define CHECK(v) do { if (!(v)) { fprintf(stderr,"CAMERA CHECK line %d: %s\n",__LINE__,#v); abort(); } } while(0)

struct camera {
    unsigned index, gets, returns, syncs, frames;
    bool pending, held, cpu;
    std::vector<uint8_t> storage;
    quest_camera_frame image;
    quest_camera_capture *capture;
};
static camera cameras[2];
static quest_camera_stream *shutters;
static quest_slam_runtime *runtime;
static uint32_t width, height;
static int64_t clock_ns;
extern "C" int quest_camera_buffer_get(int fd, int32_t session, int32_t device, quest_camera_frame *out)
{
    CHECK(fd>=91 && fd<=92 && session==17 && device==fd);
    auto &c=cameras[fd-91];CHECK(!c.held && !c.cpu);
    if(!c.pending)return -ETIMEDOUT;
    *out=c.image;c.pending=false;c.held=true;++c.gets;return 0;
}
extern "C" int quest_camera_buffer_return(int fd, int32_t session, int32_t device, uint64_t id)
{
    CHECK(fd>=91 && fd<=92 && session==17 && device==fd && id==1);
    auto &c=cameras[fd-91];CHECK(c.held && !c.cpu);c.held=false;++c.returns;
    std::memset(c.storage.data(),0xa5,c.storage.size());return 0;
}
static bool cpu_sync(void *p, void *token, bool end)
{
    auto &c=*static_cast<camera *>(p);CHECK(token==p && c.held && end==c.cpu);
    c.cpu=!end;++c.syncs;return true;
}
static bool window(void *, const quest_camera_frame *image, const quest_camera_metadata *metadata,
                   int64_t now, uint64_t *lo, uint64_t *hi)
{
    CHECK(image->qtimer_ns>0 && image->qtimer_ns<=now && metadata->tag==176);
    *lo=*hi=(uint64_t)image->qtimer_ns/1000;return true;
}
static bool center(void *, const quest_camera_frame *image, const quest_camera_metadata *,
                   const quest_camera_shutter *shutter, int64_t, int64_t *out)
{
    CHECK(shutter->timestamp_us==(uint64_t)image->qtimer_ns/1000);
    *out=image->qtimer_ns;return true;
}
static bool frame(void *p, uint64_t generation, const quest_camera_capture_frame *value, int64_t now)
{
    auto &c=*static_cast<camera *>(p);CHECK(c.held && !c.cpu && now==clock_ns);
    CHECK(value->width==width && value->height==height && value->image.frame_number==c.image.frame_number);
    CHECK(!std::memcmp(value->pixels,c.storage.data()+width,(size_t)width*height));
    pico_slam_mono8_frame f{(uint8_t)c.index,width,height,width,(size_t)width*height,value->pixels,
                           value->center_ns,(uint64_t)value->image.frame_number};
    ++c.frames;return quest_slam_runtime_camera(runtime,generation,&f);
}
static void invalidate(void *, uint64_t generation)
{ CHECK(generation==42);quest_slam_runtime_stop(runtime); }
static void stream_failed(void *, uint64_t generation, int)
{ invalidate(nullptr,generation); }
static bool shutter(void *, uint64_t generation, uint32_t bit,
                     const quest_camera_shutter_record *value, int64_t received)
{
    CHECK((bit==1 || bit==2) && value->nsync_offset_us==0 && value->timestamp_ns<=received);
    return quest_camera_capture_shutter(cameras[bit==2].capture,generation,&value->shutter,received);
}
extern "C" void quest_camera_replay_start(quest_slam_runtime *value, uint32_t w, uint32_t h, int64_t now)
{
    CHECK(!runtime && value && w>=24 && w<=4096 && h>0 && h<=4096);
    runtime=value;width=w;height=h;clock_ns=now;
    const quest_camera_capture_ops ops{cpu_sync,window,center,frame,invalidate};
    for(unsigned i=0;i<2;i++) {
        auto &c=cameras[i];c.index=i;c.storage.resize((size_t)w*(h+1));
        quest_camera_capture_config config{};
        config.fd=91+i;config.device=91+i;config.session=17;config.generation=42;config.camera_bit=1u<<i;
        config.metadata_bits=8;config.sensor=QUEST_CAMERA_OG01A1B;config.metadata={{40,41274,38425},0};
        config.layout={w,h,1,w,QUEST_CAMERA_MONO8,QUEST_CAMERA_CURVE_COPY};
        config.metadata_size=24;config.buffer_count=1;config.buffers[0]={1,&c,c.storage.data(),c.storage.size()};
        config.timeout_ns=100000000;
        CHECK(quest_camera_capture_create(&config,&ops,&c,&c.capture));CHECK(quest_camera_capture_start(c.capture,now));
    }
    quest_camera_stream_config config{};
    config.generation=42;config.camera_bits[0]=1;config.camera_bits[1]=2;config.camera_count=2;
    config.silence_ns=config.max_age_ns=100000000;config.max_gap_us=100000;
    config.shutter=shutter;config.invalidated=stream_failed;CHECK(quest_camera_stream_open(&config,&shutters));
}
extern "C" void quest_camera_replay_feed(unsigned camera, const uint8_t *pixels, size_t size,
                                          int64_t stamp, uint64_t number, int64_t now)
{
    CHECK(camera<2 && pixels && size==(size_t)width*height && stamp>0 && number<=INT64_MAX);
    auto &c=cameras[camera];CHECK(!c.pending && !c.held && !c.cpu);
    clock_ns=now;
    const uint8_t row[]={0xc0,0x2f,0xea,0xd8,0x73,0x35,0x9f,0x7f,0x0b,0x62,0x04,0x88,
                         0x00,0x93,0x4f,0x39,0xd9,0x64,0x74,0xc2,0x1a,0x55,0x12,0xb0};
    std::memcpy(c.storage.data(),row,sizeof(row));std::memcpy(c.storage.data()+width,pixels,size);
    c.image={1,stamp,stamp,(int32_t)number};c.pending=true;
}
extern "C" void quest_camera_replay_poll(int64_t now)
{
    clock_ns=now;CHECK(quest_camera_stream_poll(shutters)>=0);
    for(auto &c:cameras)CHECK(quest_camera_capture_poll(c.capture,now)>=0);
}
extern "C" void quest_camera_replay_frame(int64_t stamp, uint64_t number, int64_t now)
{
    clock_ns=now;
    if(number%2) {
        for(auto &c:cameras) CHECK(quest_camera_capture_poll(c.capture,now)==0 && c.held);
    }
    quest_replay_shutter(stamp);CHECK(quest_camera_stream_poll(shutters)==2);
    for(auto &c:cameras)CHECK(quest_camera_capture_poll(c.capture,now)==1 && !c.held);
}
extern "C" void quest_camera_replay_finish(unsigned images)
{
    quest_camera_stream_destroy(&shutters);unsigned frames=0;
    for(auto &c:cameras) {
        CHECK(!c.pending && !c.held && !c.cpu && c.gets==c.returns && c.frames==c.returns && c.syncs==2*c.returns);
        frames+=c.frames;quest_camera_capture_stop(c.capture);
        CHECK(quest_camera_capture_quiesced(c.capture));CHECK(quest_camera_capture_destroy(&c.capture));
    }
    CHECK(frames==images);runtime=nullptr;
    printf("QUEST_INSTALLED_CAPTURE_PASS images=%u shutter_fifo=1 row_decode=1 mono8_copy=1 buffer_returns=%u modeled_clock=1 modeled_dma=1 hardware=0\n",frames,frames);
}
