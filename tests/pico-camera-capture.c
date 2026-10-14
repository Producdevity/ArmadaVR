#include "pico-camera-capture.h"
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
static struct pico_camera_capture *create(struct fixture *f,size_t count)
{
    struct pico_camera_capture_config cfg={.generation=f->generation,.camera_ids={1,2,3,4},.camera_count=count,.timeout_ns=1000};
    struct pico_camera_capture *c=NULL;assert(pico_camera_capture_create(&cfg,&ops,f,&c));return c;
}
static void reserve(struct fixture *f,struct pico_camera_capture *c,unsigned i,uint64_t request,unsigned camera,int64_t now)
{
    struct pico_camera_capture_buffer b=buffer(f,i,request,camera);
    assert(!f->pending[i]&&pico_camera_capture_reserve(c,&b,now));f->pending[i]=true;
}
static enum pico_camera_capture_result timing(struct fixture *f,struct pico_camera_capture *c,uint64_t request,unsigned camera,int64_t now)
{
    return pico_camera_capture_timing(c,f->generation,request,camera,request+100,100,now);
}
int main(void)
{
    unsigned delivered=0;
    for(unsigned generation=1;generation<=1000;generation++) {
        struct fixture f={.generation=generation};size_t count=generation%2?2:4;
        struct pico_camera_capture *c=create(&f,count);
        for(unsigned request=1;request<=2;request++) {
            for(unsigned i=0;i<count;i++)reserve(&f,c,i,request,i+1,request*10);
            assert(!pico_camera_capture_quiesced(c)&&!pico_camera_capture_destroy(&c));
            for(unsigned i=0;i<count;i++) {
                assert(pico_camera_capture_fence(c,generation+1,request,200+i,2,0)==PICO_CAMERA_CAPTURE_IGNORED);
                assert(pico_camera_capture_fence(c,generation,request,999,2,0)==PICO_CAMERA_CAPTURE_IGNORED);
                if(request==2)assert(pico_camera_capture_fence(c,generation,1,200+i,3,0)==PICO_CAMERA_CAPTURE_IGNORED);
                if(i%2) {
                    assert(timing(&f,c,request,i+1,request*10+i*2+1)==PICO_CAMERA_CAPTURE_PENDING);
                    assert(timing(&f,c,request,i+1,request*10+i*2+1)==PICO_CAMERA_CAPTURE_PENDING);
                    assert(pico_camera_capture_fence(c,generation,request,200+i,2,request*10+i*2+2)==PICO_CAMERA_CAPTURE_DELIVERED);
                } else {
                    assert(pico_camera_capture_fence(c,generation,request,200+i,2,request*10+i*2+1)==PICO_CAMERA_CAPTURE_PENDING);
                    assert(pico_camera_capture_fence(c,generation,request,200+i,2,request*10+i*2+1)==PICO_CAMERA_CAPTURE_PENDING);
                    assert(timing(&f,c,request,i+1,request*10+i*2+2)==PICO_CAMERA_CAPTURE_DELIVERED);
                }
                assert(pico_camera_capture_fence(c,generation,request,200+i,2,request*10+i*2+2)==PICO_CAMERA_CAPTURE_IGNORED);
                assert(timing(&f,c,request,i+1,request*10+i*2+2)==PICO_CAMERA_CAPTURE_IGNORED);
            }
        }
        assert(f.frames==count*2&&f.syncs==f.frames*2&&f.releases==f.frames&&!f.invalidations);
        delivered+=f.frames;assert(pico_camera_capture_destroy(&c)&&!c&&f.invalidations==1);
        assert(pico_camera_capture_destroy(&c));
    }
    for(unsigned failure=1;failure<=8;failure++) {
        struct fixture f={.generation=1001,.fail=failure<=4?failure:0};struct pico_camera_capture *c=create(&f,2);
        reserve(&f,c,0,1,1,10);reserve(&f,c,1,1,2,10);
        assert(timing(&f,c,1,1,11)==PICO_CAMERA_CAPTURE_PENDING);
        if(failure<=4)assert(pico_camera_capture_fence(c,1001,1,200,2,12)==PICO_CAMERA_CAPTURE_FAILED);
        else if(failure==5)assert(pico_camera_capture_fence(c,1001,1,200,3,12)==PICO_CAMERA_CAPTURE_FAILED);
        else if(failure==6)assert(pico_camera_capture_timing(c,1001,1,1,101,101,12)==PICO_CAMERA_CAPTURE_FAILED);
        else if(failure==7)assert(!pico_camera_capture_poll(c,1010));
        else assert(!pico_camera_capture_poll(c,9));
        assert(f.invalidations==1&&f.releases==0&&f.pending[0]&&f.pending[1]);
        assert(!pico_camera_capture_destroy(&c));pico_camera_capture_stop(c);assert(f.invalidations==1);
        f.stopping=true;f.fail=4;assert(!pico_camera_capture_quiesced(c)&&f.releases==0);
        f.fail=0;assert(pico_camera_capture_quiesced(c)&&f.releases==2);assert(pico_camera_capture_destroy(&c));
    }
    struct fixture f={.generation=2000};struct pico_camera_capture *c=create(&f,2);
    for(unsigned i=0;i<16;i++)reserve(&f,c,i,i/2+1,i%2+1,1);
    struct pico_camera_capture_buffer b=buffer(&f,16,9,1);assert(!pico_camera_capture_reserve(c,&b,1));
    assert(!pico_camera_capture_destroy(&c));pico_camera_capture_stop(c);f.stopping=true;
    assert(pico_camera_capture_quiesced(c)&&f.releases==16);assert(pico_camera_capture_destroy(&c));
    f=(struct fixture){.generation=2001};c=create(&f,2);reserve(&f,c,0,1,1,1);
    b=buffer(&f,1,2,1);b.token=f.pixels[0];assert(!pico_camera_capture_reserve(c,&b,1));
    b=buffer(&f,1,2,1);b.fence=200;assert(!pico_camera_capture_reserve(c,&b,1));
    b=buffer(&f,1,2,1);b.memory_handle=100;assert(!pico_camera_capture_reserve(c,&b,1));
    b=buffer(&f,1,2,1);b.pixels=f.pixels[0]+1;assert(!pico_camera_capture_reserve(c,&b,1));
    b=buffer(&f,1,1,1);assert(!pico_camera_capture_reserve(c,&b,1));
    b=buffer(&f,1,2,3);assert(!pico_camera_capture_reserve(c,&b,1));
    b=buffer(&f,1,UINT64_MAX,1);assert(!pico_camera_capture_reserve(c,&b,1));
    b=buffer(&f,1,2,1);b.size=31;assert(!pico_camera_capture_reserve(c,&b,1));
    b=buffer(&f,1,2,1);b.stride=15;assert(!pico_camera_capture_reserve(c,&b,1));
    assert(f.releases==0&&!f.invalidations);pico_camera_capture_stop(c);f.stopping=true;
    assert(pico_camera_capture_quiesced(c)&&f.releases==1);assert(pico_camera_capture_destroy(&c));
    printf("PICO_CAMERA_CAPTURE_PASS generations=1010 delivered=%u failure_modes=8\n",delivered);
    return 0;
}
