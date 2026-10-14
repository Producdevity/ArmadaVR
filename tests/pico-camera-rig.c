#include "pico-camera-rig.h"
#include "pico-camera-buffer.h"
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

static unsigned calls, setup_calls, cleanup_calls, fail_setup, fail_cleanup, malformed, cycles, faults;
static bool cleaning, copy_handle, crm_live, device_live[2][3], hardware_live[2], link_live[2];
static unsigned hardware_count, master, configurations, memory_calls, fail_memory, buffer_count, live_buffers;
static bool streaming[2][3], active[2];
struct pico_camera_buffer { struct pico_camera_buffer_view view; bool ready, cpu, flight; };
static struct pico_camera_buffer *buffers[32];
static struct pico_camera_rig *retained[32];
static unsigned retained_count;
static int memory_failure(void) { return ++memory_calls == fail_memory ? -EIO : 0; }
int pico_camera_buffer_create(int crm, int ion, const struct pico_camera_buffer_config *c,
                              struct pico_camera_buffer **out)
{
    assert(crm == 10 && ion == -1 && buffer_count < 32 && !*out && c->cached);
    assert(c->size == 4096 || c->size == 8192);
    if (c->mapping == PICO_CAMERA_MEMORY_CPU) assert(!c->mmu_count);
    else assert(c->mapping == PICO_CAMERA_MEMORY_SHARED && c->mmu_count == 1 && c->mmus[0] == 31);
    struct pico_camera_buffer *b = calloc(1, sizeof(*b)); assert(b);
    b->view = (struct pico_camera_buffer_view){1000 + (int)buffer_count, 2000 + (int)buffer_count, calloc(1,c->size), c->size};
    assert(b->view.data); buffers[buffer_count++] = b; *out = b; ++live_buffers;
    int result = memory_failure(); b->ready = !result; return result;
}
bool pico_camera_buffer_view(const struct pico_camera_buffer *b, struct pico_camera_buffer_view *v)
{ if (!b || !b->ready) return false; *v = b->view; return true; }
int pico_camera_buffer_cpu_begin(struct pico_camera_buffer *b, enum ion_buffer_access access)
{ assert(b && b->ready && !b->flight && !b->cpu && access == ION_BUFFER_WRITE); int r=memory_failure(); if(!r)b->cpu=true; return r; }
int pico_camera_buffer_cpu_end(struct pico_camera_buffer *b)
{ assert(b && b->cpu); b->cpu=false; return memory_failure(); }
int pico_camera_buffer_submit(struct pico_camera_buffer *b)
{ assert(b && b->ready && !b->cpu && !b->flight); int r=memory_failure(); if(!r)b->flight=true; return r; }
int pico_camera_buffer_complete(struct pico_camera_buffer *b)
{ assert(b && b->flight && !b->cpu); b->flight=false; return 0; }
int pico_camera_buffer_release(struct pico_camera_buffer **out)
{
    if(!*out)return 0;
    struct pico_camera_buffer *b=*out;assert(!b->flight&&!b->cpu&&live_buffers);
    buffers[b->view.memory_handle-1000]=NULL;free(b->view.data);free(b);*out=NULL;--live_buffers;return 0;
}
static uint64_t get(const void *data, unsigned offset, unsigned size)
{
    const uint8_t *p = (const uint8_t *)data + offset; uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static void put(void *data, unsigned offset, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) ((uint8_t *)data)[offset + i] = (uint8_t)(value >> (8 * i)); }
static bool devices_alive(void)
{
    for (unsigned i = 0; i < 2; ++i)
        if (link_live[i] || hardware_live[i] || device_live[i][0] || device_live[i][1] || device_live[i][2]) return true;
    return false;
}
int ioctl(int fd, unsigned long request, ...)
{
    assert(request == 0xc01856c0UL);
    va_list args; va_start(args, request); void *header = va_arg(args, void *); va_end(args);
    assert(get(header, 8, 4) == 1 && !get(header, 12, 4));
    void *data = (void *)(uintptr_t)get(header, 16, 8);
    uint32_t op = (uint32_t)get(header, 0, 4); ++calls;
    bool failed = cleaning ? ++cleanup_calls == fail_cleanup : ++setup_calls == fail_setup;
    if (cleaning && failed) { errno = EIO; return -1; }
    if (op == 0x101) {
        assert(!cleaning && !crm_live);
        if (fd == 20 || fd == 22) {
            unsigned i = fd == 22;
            assert(get(header, 4, 4) == 40);
            put(data, 0, i + 2); put(data, 36, i + 3);
            if (malformed == 1 && !i) put(data, 0, 5);
            if (malformed == 2 && !i) put(data, 36, 5);
            if (malformed == 3 && !i) put(data, 4, 1);
        } else if (fd == 21 || fd == 23) {
            unsigned i = fd == 23;
            assert(get(header, 4, 4) == 16);
            put(data, 0, i + 3); put(data, 4, malformed == 4 ? 0 : 0x121);
        } else {
            assert(fd == 11 && get(header, 4, 4) == 16 && get(data, 0, 4) == 144);
            void *caps = (void *)(uintptr_t)get(data, 8, 8);
            put(caps, 0, malformed == 5 ? UINT32_MAX : 30); put(caps, 8, 31);
            put(caps, 16, 2); put(caps, 24, 1); put(caps, 28, 480);
            put(caps, 48, 1); put(caps, 52, 480);
        }
    } else if (op == 0x10b) {
        assert(!cleaning && fd == 10 && !crm_live); crm_live = true;
        if (!failed || copy_handle) put(data, 0, 100);
    } else if (op == 0x102) {
        assert(!cleaning && crm_live && get(data, 0, 4) == 100 && !get(data, 4, 4));
        unsigned i, kind;
        if (fd == 11) { i = hardware_count; kind = 2; assert(get(data, 12, 4) == 0xfefefefe); }
        else { assert(fd >= 20 && fd <= 23); i = (fd - 20) / 2; kind = (fd - 20) % 2; assert(!get(data, 12, 4)); }
        assert(i < 2 && !device_live[i][kind]);
        if (kind == 1) {
            const void *info = (void *)(uintptr_t)get(data, 16, 8); assert(!get(info, 0, 8));
        } else assert(!get(data, 16, 8));
        device_live[i][kind] = true;
        if (!failed || copy_handle) put(data, 4, 200 + i * 10 + kind);
    } else if (op == 0x151) {
        assert(!cleaning && fd == 11 && crm_live && get(data, 8, 4) == 100 && get(data, 20, 4) == 264);
        unsigned i = hardware_count++; assert(i < 2 && device_live[i][2]);
        assert(get(data, 12, 4) == 202 + i * 10);
        const void *resource = (void *)(uintptr_t)get(data, 24, 8);
        assert(get(resource, 24, 4) == 0x4004 + i && get(resource, 24 + 32, 4) == 0x2a);
        hardware_live[i] = true;
        put(data, 32, malformed == 7 ? 1 : 1U << i); put(data, 52, malformed == 6 ? 4 : 8); put(data, 112, 1);
    } else if (op == 0x10d) {
        assert(!cleaning && fd == 10 && crm_live && get(data, 0, 4) == 100 && get(data, 4, 4) == 2);
        unsigned i = (get(data, 8, 4) - 200) / 10;
        assert(i < 2 && device_live[i][0] && device_live[i][2] && hardware_live[i] && !link_live[i]);
        assert(get(data, 12, 4) == 202 + i * 10);
        link_live[i] = true;
        if (!failed || copy_handle) put(data, 264, 300 + i);
    } else if (op == 0x111) {
        assert(!cleaning && fd == 10 && link_live[0] && link_live[1]);
        assert(get(data, 0, 4) == 100 && get(data, 8, 4) == 2);
        assert(get(data, 12, 4) == 300 && get(data, 16, 4) == 301 && get(data, 20, 4) == 300 + master);
    } else if (op == 0x105) {
        assert(!cleaning && get(data,0,4)==100 && !get(data,8,8));
        unsigned packet_id=(unsigned)get(data,16,8)-1000;
        assert(packet_id<buffer_count && buffers[packet_id] && buffers[packet_id]->flight);
        const void *packet=buffers[packet_id]->view.data;
        unsigned opcode=(unsigned)get(packet,0,4),n=(unsigned)get(packet,28,4);
        unsigned expected[]={0x1000002,0x1000004,0x1000000,0x1000005,
                             0x1000002,0x1000004,0x1000000,0x1000005,
                             0xf000000,0xf000000,0x5000000,0x5000000};
        assert(configurations<12&&opcode==expected[configurations]);
        unsigned camera=configurations<8?configurations/4:(configurations-8)%2;
        unsigned kind=configurations<8?0:configurations<10?2:1;
        assert(fd==(kind==2?11:20+(int)camera*2+(int)kind));
        assert(get(data,4,4)==200+camera*10+kind);
        assert(n==(kind==2?2U:1U));
        for(unsigned i=0;i<n;i++) {
            unsigned id=(unsigned)get(packet,56+i*24,4)-1000;
            assert(id<buffer_count&&buffers[id]&&buffers[id]->flight&&!buffers[id]->cpu);
            if(!kind) {
                const void *command=buffers[id]->view.data;
                assert(get(packet,68+i*24,4)==16&&get(packet,72+i*24,4)==7);
                assert(get(command,0,4)==1&&get(command,8,4)==0x100&&get(command,12,4)==1+camera);
            }
        }
        ++configurations;
    } else if (op == 0x116) {
        assert(fd==10&&get(data,4,4)==100&&get(data,8,4)==1);
        unsigned camera=(unsigned)get(data,24,4)-300;assert(camera<2&&link_live[camera]);
        bool start=get(data,0,4)==0;assert(start!=cleaning);
        if(start){assert(configurations==12&&!active[camera]);active[camera]=true;}
        else {assert(active[camera]);active[camera]=false;}
    } else if (op == 0x103 || op == 0x104) {
        assert(get(data,0,4)==100);
        unsigned handle=(unsigned)get(data,4,4)-200,camera=handle/10,kind=handle%10;
        assert(camera<2&&kind<3&&device_live[camera][kind]);
        assert(fd==(kind==2?11:20+(int)camera*2+(int)kind));
        if(op==0x103){assert(!cleaning&&active[camera]&&!streaming[camera][kind]);streaming[camera][kind]=true;}
        else {assert(cleaning&&streaming[camera][kind]);streaming[camera][kind]=false;}
    } else if (op == 0x10e) {
        assert(cleaning && fd == 10 && crm_live && get(data, 0, 4) == 100 && !live_buffers);
        unsigned i = get(data, 4, 4) - 300; assert(i < 2 && link_live[i]); link_live[i] = false;
    } else if (op == 0x152) {
        assert(cleaning && fd == 11 && crm_live && get(data, 8, 4) == 100);
        unsigned i = (get(data, 12, 4) - 202) / 10;
        assert(i < 2 && !link_live[i] && hardware_live[i]); hardware_live[i] = false;
    } else if (op == 0x106) {
        assert(cleaning && crm_live && get(data, 0, 4) == 100);
        unsigned handle = get(data, 4, 4) - 200, i = handle / 10, kind = handle % 10;
        assert(i < 2 && kind < 3 && device_live[i][kind] && !link_live[i]);
        assert(fd == (kind == 2 ? 11 : 20 + (int)i * 2 + (int)kind));
        if (kind == 2) assert(!hardware_live[i]);
        device_live[i][kind] = false;
    } else if (op == 0x10c) {
        assert(cleaning && fd == 10 && get(data, 0, 4) == 100 && crm_live && !devices_alive()); crm_live = false;
    } else assert(!"unexpected ioctl");
    if (failed) { errno = EINTR; return -1; }
    return 0;
}
static struct pico_camera_session_config session_config(void)
{
    struct pico_camera_session_config c = {.crm_fd = 10, .ife_fd = 11, .master = master};
    for (unsigned i = 0; i < 2; ++i) {
        c.cameras[i].sensor_fd = 20 + i * 2; c.cameras[i].phy_fd = 21 + i * 2;
        c.cameras[i].sensor_slot = 2 + i; c.cameras[i].phy_version = 0x121;
        c.cameras[i].input = (struct pico_camera_ife_config){.phy = i + 3, .lanes = 2, .lane_map = 0x3210,
            .virtual_channel = 0, .data_type = 0x2a, .width = 640, .height = 400, .bits = 8, .output = PICO_CAMERA_RAW_MIPI};
    }
    return c;
}

static struct pico_camera_rig_config config(void)
{
    static struct pico_camera_sensor_write writes[2]={{0x100,1,0},{0x100,2,0}};
    struct pico_camera_rig_config c={.session=session_config(),.command_mapping=PICO_CAMERA_MEMORY_SHARED};
    for(unsigned i=0;i<2;i++){
        struct pico_camera_sensor_table table={{2,1,1,false,0xe2+i*2},writes+i,1};
        c.cameras[i].initialize=c.cameras[i].mode=c.cameras[i].stream_on=c.cameras[i].stream_off=table;
        c.cameras[i].phy=(struct pico_camera_csiphy_config){0,0,2,0x3210,14,384000000,0};
        c.cameras[i].clocks=(struct pico_camera_ife_clocks){400000000,0,200000000,false};
        c.cameras[i].bandwidth=(struct pico_camera_ife_bandwidth){640000000,800000000,1000000000};
    }
    return c;
}
static void reset(void)
{
    calls=setup_calls=cleanup_calls=hardware_count=configurations=memory_calls=buffer_count=live_buffers=0;
    fail_setup=fail_cleanup=fail_memory=malformed=0;copy_handle=true;cleaning=crm_live=false;
    memset(device_live,0,sizeof(device_live));memset(hardware_live,0,sizeof(hardware_live));
    memset(link_live,0,sizeof(link_live));memset(active,0,sizeof(active));memset(streaming,0,sizeof(streaming));
    memset(buffers,0,sizeof(buffers));master=cycles%2;
}
static void run(unsigned io_failure,unsigned memory_error,unsigned cleanup_error)
{
    reset();fail_setup=io_failure;fail_memory=memory_error;fail_cleanup=cleanup_error;
    struct pico_camera_rig_config c=config();struct pico_camera_rig *d=NULL;
    int result=pico_camera_rig_prepare(&c,&d);assert(d);
    struct pico_camera_session_view view;
    if(memory_error||(io_failure&&io_failure<=29)){
        assert(result==(memory_error?-EIO:-EINTR));
        assert(!pico_camera_rig_view(d,&view)&&pico_camera_rig_start(d)==-EINVAL);
    }else{
        assert(!result&&pico_camera_rig_view(d,&view)&&view.session==100);
        assert(configurations==12&&memory_calls==104);
        assert(pico_camera_rig_prepare(&c,&d)==-EINVAL);
        memset(&c,0xa5,sizeof(c));result=pico_camera_rig_start(d);
        assert(result==(io_failure?-EINTR:0));
        assert(pico_camera_rig_start(d)==-EINVAL);
    }
    assert(pico_camera_rig_release(&d)==-EBUSY);
    assert(pico_camera_rig_quiesced(d)==-EINVAL);
    cleaning=true;result=pico_camera_rig_stop(d);
    unsigned stopped_calls=calls;
    assert(pico_camera_rig_stop(d)==result&&stopped_calls==calls);
    assert(!pico_camera_rig_view(d,&view)&&pico_camera_rig_start(d)==-EINVAL);
    if(cleanup_error&&cleanup_error<=8){
        assert(result==-EIO&&pico_camera_rig_quiesced(d)==-EIO);
        assert(pico_camera_rig_release(&d)==-EBUSY);
    }else{
        assert(!result&&!pico_camera_rig_quiesced(d));
        assert(pico_camera_rig_quiesced(d)==-EINVAL);
        result=pico_camera_rig_release(&d);
        if(cleanup_error){assert(result==-EIO&&d);unsigned before=calls;assert(pico_camera_rig_release(&d)==-EIO&&before==calls);}
        else assert(!result&&!d&&!crm_live&&!devices_alive()&&!live_buffers);
    }
    if(d){assert(retained_count<32);retained[retained_count++]=d;}
    if(io_failure||memory_error||cleanup_error)++faults;else++cycles;
}
int main(void)
{
    for(unsigned i=0;i<200;i++)run(0,0,0);
    for(unsigned i=1;i<=104;i++)run(0,i,0);
    for(unsigned i=1;i<=37;i++)run(i,0,0);
    for(unsigned i=1;i<=19;i++)run(0,0,i);
    reset();struct pico_camera_rig_config c=config();struct pico_camera_rig *d=NULL;
#define BAD(field,value) do {struct pico_camera_rig_config bad=c;bad.field=value; \
    assert(pico_camera_rig_prepare(&bad,&d)==-EINVAL&&!d&&!calls&&!memory_calls);}while(0)
    BAD(command_mapping,PICO_CAMERA_MEMORY_CPU);BAD(cameras[0].phy.lanes,1);
    BAD(cameras[0].phy.lane_map,0x2301);BAD(cameras[0].phy.cphy,1);BAD(cameras[0].phy.combo,1);
    BAD(cameras[0].phy.data_rate,0);BAD(cameras[0].sample_pattern,8);BAD(cameras[0].clocks.dual,true);
    BAD(cameras[0].initialize.count,0);BAD(cameras[1].stream_off.writes,NULL);
    BAD(session.cameras[0].input.output,PICO_CAMERA_RAW_PLAIN8);
    assert(pico_camera_rig_prepare(NULL,&d)==-EINVAL);
    assert(pico_camera_rig_prepare(&c,NULL)==-EINVAL);
    assert(!pico_camera_rig_view(NULL,NULL));assert(pico_camera_rig_start(NULL)==-EINVAL);
    assert(pico_camera_rig_stop(NULL)==-EINVAL&&pico_camera_rig_quiesced(NULL)==-EINVAL);
    assert(pico_camera_rig_release(NULL)==-EINVAL&&!pico_camera_rig_release(&d));
    printf("PICO_CAMERA_RIG_PASS cycles=%u faults=%u retained=%u hardware=0\n",cycles,faults,retained_count);
    return retained[0]?0:1;
}
