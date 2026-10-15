#include "pico-camera-transport.h"
#include "pico-camera-stream.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

struct pico_camera_buffer {
    struct pico_camera_buffer_view view;
    bool ready, flight, cpu;
};
static struct pico_camera_buffer *buffers[64];
static unsigned next_buffer, live_buffers, io_calls, fail_io_at, memory_calls, fail_memory_at;
static unsigned frames, invalidations, fences, fence_destroys;
static unsigned event_head[2],event_tail[2],sequence[2],event_error;
static unsigned frame_failure, stale_cookie, corrupt_sequence, pending_camera;
static uint64_t request_id;
static int64_t event_time;
static unsigned clock_calls, clock_fault;
static int64_t transport_clock(void *opaque)
{
    assert(opaque==&clock_calls);++clock_calls;
    if(clock_calls==2) {
        if(clock_fault==1)return 0;
        if(clock_fault==2)return 199;
        if(clock_fault==3)return 1100;
    }
    return 190+10*clock_calls;
}
static struct pico_camera_event events[2][32];
static struct pico_camera_transport *volatile retained;
static uint64_t get(const void *data, unsigned offset, unsigned size)
{
    const uint8_t *p = (const uint8_t *)data + offset; uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static void set(void *data, unsigned offset, uint64_t value, unsigned size)
{
    uint8_t *p = (uint8_t *)data + offset;
    for (unsigned i = 0; i < size; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
static int memory_failure(void) { return ++memory_calls == fail_memory_at ? -EIO : 0; }
int pico_camera_buffer_create(int crm, int ion, const struct pico_camera_buffer_config *c,
                              struct pico_camera_buffer **out)
{
    assert(crm == 31 && ion == 32 && out && !*out && next_buffer < 64);
    struct pico_camera_buffer *b = calloc(1, sizeof(*b)); assert(b);
    *out = b; unsigned id = next_buffer++; buffers[id] = b; ++live_buffers;
    b->view = (struct pico_camera_buffer_view){100 + (int)id, 200 + (int)id, calloc(1, c->size), c->size};
    assert(b->view.data);
    switch (id % 4) {
    case 0: assert(c->kind == PICO_CAMERA_BUFFER_PIXELS && c->mapping == PICO_CAMERA_MEMORY_IO);
        assert(c->mmu_count == 1 && c->mmus[0] == 71 && c->ion_heap == 25 && c->size == 8192); break;
    case 1: case 2: assert(c->kind == PICO_CAMERA_BUFFER_PACKET && c->mapping == PICO_CAMERA_MEMORY_CPU);
        assert(!c->mmu_count && c->size == 4096); break;
    case 3: assert(c->kind == PICO_CAMERA_BUFFER_COMMAND && c->mapping == PICO_CAMERA_MEMORY_SHARED);
        assert(c->mmu_count == 1 && c->mmus[0] == 72 && c->size == 4096); break;
    }
    int result = memory_failure(); b->ready = !result; return result;
}
bool pico_camera_buffer_view(const struct pico_camera_buffer *b, struct pico_camera_buffer_view *v)
{ if (!b || !b->ready) return false; *v = b->view; return true; }
int pico_camera_buffer_cpu_begin(struct pico_camera_buffer *b, enum ion_buffer_access access)
{
    assert(b && b->ready && !b->flight && !b->cpu && (access == ION_BUFFER_READ || access == ION_BUFFER_WRITE));
    int result = memory_failure(); if (!result) b->cpu = true; return result;
}
int pico_camera_buffer_cpu_end(struct pico_camera_buffer *b)
{ assert(b && b->cpu); b->cpu = false; return memory_failure(); }
int pico_camera_buffer_submit(struct pico_camera_buffer *b)
{ assert(b && b->ready && !b->flight && !b->cpu); int result = memory_failure(); if (!result) b->flight = true; return result; }
int pico_camera_buffer_complete(struct pico_camera_buffer *b)
{ assert(b && b->flight && !b->cpu); b->flight = false; return 0; }
int pico_camera_buffer_release(struct pico_camera_buffer **out)
{
    if (!*out) return 0;
    struct pico_camera_buffer *b = *out;
    if (b->flight || b->cpu) return -EBUSY;
    assert(live_buffers); --live_buffers; buffers[b->view.memory_handle - 100] = NULL;
    memset(b->view.data, 0xa5, b->view.size); free(b->view.data); free(b); *out = NULL; return 0;
}
int ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request); uint8_t *control = va_arg(args, void *); va_end(args);
    if (request == 0x80885659UL) {
        assert(fd == 31 || fd == 33); unsigned source = fd == 31;
        if (event_error) { errno = EINTR; return -1; }
        if (event_head[source] == event_tail[source]) { errno = EAGAIN; return -1; }
        struct pico_camera_event *e = &events[source][event_head[source]++];
        set(control,0,e->type,4); memcpy(control+8,e->data,64); set(control,76,e->sequence,4);set(control,96,e->id,4);
        return 0;
    }
    assert(request == 0xc01856c0UL);
    unsigned opcode = (unsigned)get(control,0,4); void *data = (void *)(uintptr_t)get(control,16,8);
    if (++io_calls == fail_io_at) { errno = EINTR; return -1; }
    if (fd == 33) {
        if (!opcode) set(data,64,50 + ++fences,4);
        else if (opcode == 4) assert(get(data,8,8) == 17);
        else { assert(opcode == 1); ++fence_destroys; }
    } else if (fd == 31) {
        assert(opcode == 0x10f); pending_camera = get(data,4,4) == 22;
        assert(get(data,4,4) == 21 + pending_camera); request_id = get(data,24,8);
    } else {
        assert(fd >= 34 && fd <= 37 && opcode == 0x105);
        assert(get(data,4,4) == 41 + pending_camera * 2 + (fd % 2));
        unsigned id = (unsigned)get(data,16,8) - 100;
        assert(id < 64 && buffers[id] && buffers[id]->flight);
        const uint8_t *packet = buffers[id]->view.data;
        assert(get(packet,8,8) == request_id);
        if (fd % 2) {
            unsigned image = (unsigned)get(packet,80,4) - 100;
            assert(image < 64 && buffers[image]->flight);
            memset(buffers[image]->view.data,0x5a,buffers[image]->view.size);
            assert(event_tail[0]<32 && event_tail[1]<32);
            struct pico_camera_event *f=&events[0][event_tail[0]++], *s=&events[1][event_tail[1]++];
            *f=(struct pico_camera_event){.type=0x08000000,.sequence=sequence[0]++};
            *s=(struct pico_camera_event){.type=0x08000000,.sequence=sequence[1]++};
            if (corrupt_sequence && pending_camera) s->sequence++;
            set(f->data,0,get(packet,272,4),4);set(f->data,4,2,4);set(f->data,8,stale_cookie?16:17,8);set(f->data,16,request_id,8);
            set(s->data,0,11,4);set(s->data,32,21+pending_camera,4);set(s->data,8,request_id,8);
            set(s->data,16,request_id,8);set(s->data,24,event_time,8);
        }
    }
    return 0;
}
static bool frame(void *opaque, uint64_t generation, const struct pico_camera_capture_frame *f, int64_t now)
{
    assert(opaque == &frames && generation == 17 && now > 0 && f->buffer.pixels[0] == 0x5a);
    assert(f->center_ns == event_time - (f->buffer.camera_id == 7 ? 10 : 20));
    assert(now >= f->center_ns);
    ++frames;return !frame_failure;
}
static void invalid(void *opaque,uint64_t generation)
{ assert(opaque == &frames && generation == 17);++invalidations; }
static struct pico_camera_transport *create(void)
{
    assert(!live_buffers);next_buffer = io_calls = memory_calls = fail_io_at = fail_memory_at = 0;
    event_head[0]=event_head[1]=event_tail[0]=event_tail[1]=0;
    sequence[0]=sequence[1]=UINT32_MAX-1;
    frame_failure=stale_cookie=corrupt_sequence=event_error=0;event_time=200;
    struct pico_camera_transport_config c={.timeout_ns=1000};
    for(unsigned i=0;i<2;i++)c.cameras[i]=(struct pico_camera_request_config){
        .crm_fd=31,.sync_fd=33,.ion_fd=32,.sensor_fd=34+(int)i*2,.ife_fd=35+(int)i*2,
        .session=11,.link=21+(int)i,.sensor=41+(int)i*2,.ife=42+(int)i*2,
        .device_mmu=71,.command_mmu=72,.command_mapping=PICO_CAMERA_MEMORY_SHARED,
        .ion_heap=25,.width=65,.height=67,.stride=80,.camera_id=7+i,
        .generation=17,.request_id=UINT64_C(0x100000001),.synchronized=true};
    struct pico_camera_transport_ops ops={frame,invalid};struct pico_camera_transport *t=NULL;
    assert(!pico_camera_transport_create(&c,&ops,&frames,&t));memset(&c,0xa5,sizeof(c));return t;
}
static void cleanup(struct pico_camera_transport **t)
{
    fail_memory_at=fail_io_at=0;
    unsigned before=invalidations;
    assert(!pico_camera_transport_quiesced(*t));assert(!live_buffers);
    assert(!pico_camera_transport_quiesced(*t));assert(invalidations<=before+1);
    assert(!pico_camera_transport_destroy(t) && !*t);
}
int main(void)
{
    const int64_t offsets[2]={-10,-20};unsigned successful=0,faults=0;
    for(unsigned cycle=0;cycle<100;cycle++) {
        struct pico_camera_transport *t=create();
        for(unsigned batch=0;batch<3;batch++) {
            for(unsigned pair=0;pair<8;pair++)assert(!pico_camera_transport_queue(t,offsets,event_time-100));
            assert(pico_camera_transport_queue(t,offsets,event_time-100)==-EAGAIN);
            assert(pico_camera_transport_destroy(&t)==-EBUSY);
            unsigned before=frames;assert(pico_camera_transport_poll(t,event_time)==32);
            assert(frames==before+16 && !live_buffers);successful+=16;
            assert(!pico_camera_transport_poll(t,event_time));
            next_buffer=0;event_head[0]=event_head[1]=event_tail[0]=event_tail[1]=0;event_time+=1000;
        }
        assert(!pico_camera_transport_destroy(&t));
    }
    for(unsigned fault=1;fault<=28;fault++) {
        struct pico_camera_transport *t=create();fail_memory_at=fault;
        assert(pico_camera_transport_queue(t,offsets,100)==-EIO);
        assert(pico_camera_transport_poll(t,200)==-EIO);cleanup(&t);++faults;
    }
    for(unsigned fault=1;fault<=10;fault++) {
        struct pico_camera_transport *t=create();fail_io_at=fault;
        assert(pico_camera_transport_queue(t,offsets,100)==-EINTR);cleanup(&t);++faults;
    }
    for(unsigned fault=0;fault<5;fault++) {
        struct pico_camera_transport *t=create();
        if(fault==0)stale_cookie=1;
        if(fault==1)corrupt_sequence=1;
        assert(!pico_camera_transport_queue(t,offsets,100));
        if(fault==2)frame_failure=1;
        if(fault==3)event_error=1;
        int result=pico_camera_transport_poll(t,fault==4?1100:200);
        if(fault==0) { assert(result==4 && live_buffers==8);assert(pico_camera_transport_poll(t,1100)==-EIO); }
        else assert(result==(fault==3?-EINTR:-EIO));
        cleanup(&t);++faults;
    }
    for(unsigned n=0;n<4;n++) {
        struct pico_camera_transport *clocked=create();event_time=230;
        assert(!pico_camera_transport_queue(clocked,offsets,100));clock_calls=0;clock_fault=n;
        unsigned before=frames;
        int result=pico_camera_transport_poll_clock(clocked,transport_clock,&clock_calls);
        if(!n)assert(result==4 && frames==before+2 && clock_calls==7 && !live_buffers);
        else assert(result==-EIO && frames==before && clock_calls==2);
        cleanup(&clocked);
    }
    clock_fault=3;clock_calls=0;
    struct pico_camera_transport *silent=create();assert(!pico_camera_transport_queue(silent,offsets,100));
    event_head[0]=event_tail[0];event_head[1]=event_tail[1];
    assert(pico_camera_transport_poll_clock(silent,transport_clock,&clock_calls)==-EIO && clock_calls==2);
    cleanup(&silent);
    printf("PICO_CAMERA_TRANSPORT_CLOCK_PASS cases=5 dequeues_refreshed=1 silent_timeout=1\n");
    struct pico_camera_transport *t=create();assert(!pico_camera_transport_queue(t,offsets,100));
    fail_io_at=io_calls+1;assert(pico_camera_transport_poll(t,200)==-EIO);
    assert(pico_camera_transport_quiesced(t)==-EIO && live_buffers);
    unsigned calls=io_calls;assert(pico_camera_transport_quiesced(t)==-EIO && calls==io_calls);
    retained=t;
    assert(successful==4800);
    printf("PICO_CAMERA_TRANSPORT_PASS frames=%u faults=%u uncertain_fence_retained=1\n",successful,faults+1);
}
