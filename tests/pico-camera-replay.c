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

enum { CRM=1000031, SYNC=1000033 };
struct pico_camera_buffer { struct pico_camera_buffer_view view; bool flight, cpu; enum pico_camera_buffer_kind kind; };
static struct pico_camera_buffer *buffers[8], *images[2];
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
    assert(crm==CRM && ion==1000032 && out && !*out && c->size && !(c->size%4096));
    unsigned i=0;while(i<8 && buffers[i])++i;assert(i<8);
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
int ioctl(int fd,unsigned long request,...)
{
    va_list args;va_start(args,request);void *data=va_arg(args,void *);va_end(args);
    if(fd<1000030 || fd>1000037)return (int)syscall(SYS_ioctl,fd,request,data);
    if(request==0x80885659UL) {
        assert(fd==CRM||fd==SYNC);unsigned source=fd==CRM;
        if(head[source]==tail[source]) { errno=EAGAIN;return -1; }
        struct pico_camera_event *e=&events[source][head[source]++];
        put(data,0,e->type,4);memcpy((uint8_t *)data+8,e->data,64);put(data,76,e->sequence,4);put(data,96,e->id,4);return 0;
    }
    assert(request==0xc01856c0UL);
    unsigned opcode=get(data,0,4);void *arg=(void *)(uintptr_t)get(data,16,8);
    if(fd==SYNC) {
        if(!opcode)put(arg,64,++next_fence,4);
        else if(opcode==4)assert(get(arg,8,8)==1);
        else { assert(opcode==1);++destroyed_fences; }
    } else if(fd==CRM)assert(opcode==0x10f);
    else {
        assert(fd>=1000034 && opcode==0x105);unsigned camera=(fd-1000034)/2;
        struct pico_camera_buffer *packet=buffers[get(arg,16,8)-100];assert(packet&&packet->flight);
        if(fd%2) {
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
    for(unsigned i=0;i<8;++i)assert(!buffers[i]);
    assert(next_fence==destroyed_fences && next_fence==frame_releases && head[0]==tail[0] && head[1]==tail[1]);
    return frame_releases;
}
