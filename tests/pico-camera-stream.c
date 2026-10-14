#include "pico-camera-stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    unsigned char pixels[17][32];
    unsigned syncs, frames, releases, invalidations, phase, fail;
    uint64_t generation;
    bool pending[17], stopping;
};
static unsigned index_of(struct fixture *f, void *token)
{
    for (unsigned i=0;i<17;i++) if(token==f->pixels[i]) return i;
    assert(false);return 0;
}
static bool sync_cpu(void *opaque, void *token, bool end)
{
    struct fixture *f=opaque;assert(f->pending[index_of(f,token)]);f->syncs++;
    assert(end?f->phase==2:f->phase==0);f->phase=end?3:1;
    return f->fail!=(end?3U:1U);
}
static bool frame(void *opaque, uint64_t generation, const struct pico_camera_capture_frame *p, int64_t now)
{
    struct fixture *f=opaque;assert(f->phase==1&&generation==f->generation&&now>0);f->phase=2;
    assert(p->center_ns==100&&p->frame_number==p->buffer.request_id+100);
    assert(p->buffer.width==2&&p->buffer.height==2&&p->buffer.stride==16&&p->buffer.size==32);
    assert(p->buffer.pixels[0]==7&&p->buffer.pixels[1]==8&&p->buffer.pixels[16]==9&&p->buffer.pixels[17]==10);
    f->frames++;return f->fail!=2;
}
static bool release(void *opaque, void *token)
{
    struct fixture *f=opaque;unsigned i=index_of(f,token);assert(f->pending[i]);
    assert(f->stopping||f->phase==3);if(f->fail==4)return false;
    f->pending[i]=false;memset(f->pixels[i],0xdd,32);f->releases++;f->phase=0;return true;
}
static void invalidate(void *opaque,uint64_t generation)
{
    struct fixture *f=opaque;assert(generation==f->generation);f->invalidations++;
}
static const struct pico_camera_capture_ops ops={sync_cpu,frame,release,invalidate};
static struct pico_camera_capture_buffer buffer(struct fixture *f,unsigned i,uint64_t request,unsigned camera)
{
    memset(f->pixels[i],0xcc,32);f->pixels[i][0]=7;f->pixels[i][1]=8;f->pixels[i][16]=9;f->pixels[i][17]=10;
    return (struct pico_camera_capture_buffer){.token=f->pixels[i],.pixels=f->pixels[i],.size=32,
        .request_id=request,.memory_handle=100+(int)i,.fence=200+(int)i,.camera_id=camera,
        .width=2,.height=2,.stride=16};
}
static void put(uint8_t *p, unsigned offset, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i) p[offset + i] = (uint8_t)(value >> (8 * i));
}
static struct pico_camera_event fence_event(uint64_t generation, uint64_t request,
                                            unsigned index, uint32_t sequence)
{
    struct pico_camera_event e = {.type=0x08000000,.sequence=sequence};
    put(e.data,0,200+index,4);put(e.data,4,2,4);put(e.data,8,generation,8);put(e.data,16,request,8);
    return e;
}
static struct pico_camera_event sof_event(uint64_t request, unsigned index, uint32_t sequence)
{
    struct pico_camera_event e = {.type=0x08000000,.sequence=sequence};
    put(e.data,0,77,4);put(e.data,8,request,8);put(e.data,16,request+100,8);
    put(e.data,24,200,8);put(e.data,32,100+index,4);return e;
}
static struct pico_camera_stream *create(struct fixture *f, size_t count)
{
    struct pico_camera_stream_config cfg = {
        .capture={.generation=f->generation,.camera_ids={1,2,3,4},.camera_count=count,.timeout_ns=1000},
        .session=77,.links={100,101,102,103},.first_request_id=100};
    struct pico_camera_stream *s=NULL;assert(pico_camera_stream_create(&cfg,&ops,f,&s));return s;
}
static void reserve(struct fixture *f, struct pico_camera_stream *s, unsigned index,
                     uint64_t request, unsigned camera, int64_t offset)
{
    struct pico_camera_capture_buffer b=buffer(f,index,request,camera);
    assert(!f->pending[index]&&pico_camera_stream_reserve(s,&b,offset,10));f->pending[index]=true;
}
int main(void)
{
    unsigned delivered=0;
    for(unsigned generation=1;generation<=200;generation++) {
        struct fixture f={.generation=generation};size_t count=generation%2?2:4;
        struct pico_camera_stream *s=create(&f,count);uint32_t sequence[2]={UINT32_MAX-2,UINT32_MAX-2};
        for(unsigned request=100;request<102;request++) {
            for(unsigned i=0;i<count;i++)reserve(&f,s,i,request,i+1,-100);
            assert(!pico_camera_stream_quiesced(s)&&!pico_camera_stream_destroy(&s));
            for(unsigned i=0;i<count;i++) {
                struct pico_camera_event e=fence_event(generation+1,request,i,sequence[0]++);
                assert(pico_camera_stream_event(s,PICO_CAMERA_SYNC_EVENTS,&e,10)==PICO_CAMERA_CAPTURE_IGNORED);
                e=sof_event(99,i,sequence[1]++);
                assert(pico_camera_stream_event(s,PICO_CAMERA_CRM_EVENTS,&e,10)==PICO_CAMERA_CAPTURE_IGNORED);
                e=sof_event(999,i,sequence[1]++);
                assert(pico_camera_stream_event(s,PICO_CAMERA_CRM_EVENTS,&e,10)==PICO_CAMERA_CAPTURE_IGNORED);
                for(unsigned phase=0;phase<2;phase++) {
                    bool sync=(i+phase)%2;
                    e=sync?fence_event(generation,request,i,sequence[0]++):sof_event(request,i,sequence[1]++);
                    assert(pico_camera_stream_event(s,sync?PICO_CAMERA_SYNC_EVENTS:PICO_CAMERA_CRM_EVENTS,&e,10)==
                           (phase?PICO_CAMERA_CAPTURE_DELIVERED:PICO_CAMERA_CAPTURE_PENDING));
                }
            }
        }
        assert(f.frames==count*2&&f.releases==f.frames&&f.syncs==f.frames*2&&!f.invalidations);
        delivered+=f.frames;assert(pico_camera_stream_destroy(&s)&&!s&&f.invalidations==1);
    }
    for(unsigned fault=0;fault<12;fault++) {
        struct fixture f={.generation=1000};struct pico_camera_stream *s=create(&f,2);
        reserve(&f,s,0,100,1,fault==9?100:-100);reserve(&f,s,1,100,2,-100);
        struct pico_camera_event e=sof_event(100,0,17);
        enum pico_camera_event_source source=PICO_CAMERA_CRM_EVENTS;
        if(fault==0)put(e.data,36,1,4);
        else if(fault==1){e.id=1;memset(e.data,0,64);put(e.data,0,77,4);put(e.data,8,3,4);put(e.data,12,UINT32_MAX,4);put(e.data,20,100,4);}
        else if(fault==2){e=fence_event(1000,100,0,0);put(e.data,4,3,4);source=PICO_CAMERA_SYNC_EVENTS;}
        else if(fault==3){e=fence_event(0,100,0,0);source=PICO_CAMERA_SYNC_EVENTS;}
        else if(fault==4)put(e.data,24,0,8);
        else if(fault==5||fault==6){assert(pico_camera_stream_event(s,source,&e,10)==PICO_CAMERA_CAPTURE_PENDING);e.sequence+=fault==5?2:0;}
        else if(fault==7){e=fence_event(1000,100,0,UINT32_MAX);assert(pico_camera_stream_event(s,PICO_CAMERA_SYNC_EVENTS,&e,10)==PICO_CAMERA_CAPTURE_PENDING);e.sequence=1;source=PICO_CAMERA_SYNC_EVENTS;}
        else if(fault==8)put(e.data,24,100,8);
        else if(fault==9)put(e.data,24,INT64_MAX,8);
        else if(fault==10){assert(pico_camera_stream_event(s,source,&e,10)==PICO_CAMERA_CAPTURE_PENDING);e=sof_event(100,0,18);put(e.data,24,201,8);}
        else {f.fail=4;assert(pico_camera_stream_event(s,source,&e,10)==PICO_CAMERA_CAPTURE_PENDING);e=fence_event(1000,100,0,0);source=PICO_CAMERA_SYNC_EVENTS;}
        assert(pico_camera_stream_event(s,source,&e,10)==PICO_CAMERA_CAPTURE_FAILED);
        assert(f.invalidations==1&&!f.releases&&!pico_camera_stream_destroy(&s));
        pico_camera_stream_stop(s);assert(f.invalidations==1);f.stopping=true;f.fail=4;
        assert(!pico_camera_stream_quiesced(s)&&!f.releases);f.fail=0;
        assert(pico_camera_stream_quiesced(s)&&f.releases==2);assert(pico_camera_stream_destroy(&s));
    }
    struct fixture f={.generation=2000};struct pico_camera_stream *s=create(&f,2);
    reserve(&f,s,0,100,1,-100);struct pico_camera_capture_buffer b=buffer(&f,1,99,2);
    assert(!pico_camera_stream_reserve(s,&b,-100,10));b.request_id=100;
    assert(!pico_camera_stream_reserve(s,&b,INT64_MIN,10));
    struct pico_camera_event e=sof_event(100,0,0);put(e.data,0,78,4);
    assert(pico_camera_stream_event(s,PICO_CAMERA_CRM_EVENTS,&e,10)==PICO_CAMERA_CAPTURE_IGNORED);
    e=sof_event(100,0,1);put(e.data,32,999,4);
    assert(pico_camera_stream_event(s,PICO_CAMERA_CRM_EVENTS,&e,10)==PICO_CAMERA_CAPTURE_IGNORED);
    e.id=2;e.sequence=2;
    assert(pico_camera_stream_event(s,PICO_CAMERA_CRM_EVENTS,&e,10)==PICO_CAMERA_CAPTURE_IGNORED);
    assert(!f.invalidations&&!pico_camera_stream_poll(s,1010)&&f.invalidations==1);
    f.stopping=true;assert(pico_camera_stream_quiesced(s)&&pico_camera_stream_destroy(&s));
    printf("PICO_CAMERA_STREAM_PASS delivered=%u faults=12 generations=213 hardware=0\n",delivered);return 0;
}
