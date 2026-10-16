#define _GNU_SOURCE
#include "quest-camera-pool.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef QUEST_CAMERA_POOL_INSTALLED
#include <sys/syscall.h>
#define quest_test_ioctl ioctl
#define quest_test_fcntl fcntl
#define quest_test_mmap mmap
#define quest_test_munmap munmap
#define quest_test_close close
#endif
static struct { unsigned char data[8192]; size_t size; bool fd,handle,mapped; unsigned cpu; } buffers[90];
static unsigned next_buffer,operations,fail_at,phase,submitted,count,cycles,faults,cache_calls;
static bool partial,lost,quiesced,duplicate,shared,integrating,held;
static unsigned capture_index,capture_frames,capture_returns,invalidations;
static uint64_t get(const void *data,unsigned offset,unsigned size)
{ const unsigned char *p=(const unsigned char *)data+offset;uint64_t v=0;for(unsigned i=0;i<size;i++)v|=(uint64_t)p[i]<<(8*i);return v; }
static void put(void *data,unsigned offset,uint32_t v)
{ for(unsigned i=0;i<4;i++)((unsigned char *)data)[offset+i]=(unsigned char)(v>>(8*i)); }
static bool failure(void){return ++operations==fail_at;}
static int failed(void){errno=EINTR;return -1;}
static uint32_t handle(unsigned i){return ((100+i)<<16)|(i+1);}
static unsigned handle_index(uint32_t h)
{ unsigned i=(h&0x7fff)-1;assert(i<next_buffer && h==handle(i));return i; }
static void verify_stream(void *data)
{
    assert(get(data,0,4)==41 && get(data,4,4)==42 && get(data,8,4)==count && !get(data,12,4));
    for(unsigned i=0;i<count;i++) {
        const unsigned char *item=(const unsigned char *)data+16+i*48;
        assert(get(item,0,8)==handle(i*3) && !get(item,8,8) && !get(item,16,8));
        assert(get(item,24,8)==i+1 && get(item,32,8)==handle(i*3+2) && !get(item,40,8));
        const unsigned char *p=buffers[i*3+2].data;
        assert(get(p,0,4)==0xf000001 && get(p,4,4)==336 && !get(p,8,8));
        assert(get(p,56,4)==handle(i*3+1) && get(p,64,4)==buffers[i*3+1].size && !get(p,68,8));
        assert(get(p,80,4)==handle(i*3) && get(p,80+24,4)==(integrating?32U:40U) && get(p,80+28,4)==33);
        assert(get(p,80+32,4)==64 && get(p,80+36,4)==33 && get(p,80+176,4)==5 && !get(p,80+192,4));
        for(unsigned n=336;n<4096;n++)assert(!p[n]);
        for(unsigned r=0;r<3;r++) {
            unsigned b=i*3+r;assert(buffers[b].fd && buffers[b].handle && buffers[b].mapped && !buffers[b].cpu);
            if(r!=2)for(unsigned n=0;n<buffers[b].size;n++)assert(!buffers[b].data[n]);
        }
    }
}
int quest_test_ioctl(int fd,unsigned long request,...)
{
    va_list ap;va_start(ap,request);void *p=va_arg(ap,void *);va_end(ap);bool fail=failure();
    if(request==0x40086200UL) {
        assert(fd>=100 && (unsigned)fd<100+next_buffer);
        unsigned i=fd-100;assert(buffers[i].fd && buffers[i].mapped);
        unsigned flags=get(p,0,8);assert(phase==0 || phase==2);assert((flags&3)==(phase==0?2U:1U));
        if(flags&4){assert(buffers[i].cpu==(flags&3));buffers[i].cpu=0;}
        else {assert(!buffers[i].cpu);if(!fail)buffers[i].cpu=flags&3;}
        if(phase==2){assert(i%3==0);if(integrating)assert(held && i/3==capture_index%count);cache_calls++;}
        return fail?failed():0;
    }
    if(request==0xc0184800UL) {
        assert(!phase && fd==33 && next_buffer<90 && next_buffer%3==0);
        assert(get(p,0,8)==4096 && !get(p,8,4) && get(p,12,4)==0x80002 && !get(p,16,8));
        unsigned i=next_buffer++;buffers[i].fd=true;buffers[i].size=4096;
        if(!fail || partial)put(p,8,100+i);else lost=true;
        return fail?failed():0;
    }
    assert(request==0xc01856c0UL && get(p,8,4)==1 && !get(p,12,4));
    unsigned op=get(p,0,4);void *data=(void *)(uintptr_t)get(p,16,8);
    if(op==0x116) {
        assert(!phase && fd==31 && next_buffer<90 && next_buffer%3);
        unsigned i=next_buffer++;bool packet=i%3==2;buffers[i].size=packet?4096:8192;
        assert(get(data,0,8)==buffers[i].size && get(data,8,8)==4096);
        assert(get(data,80,4)==(packet?0U:1U) && get(data,16,4)==(packet?0U:55U));
        assert(get(data,84,4)==(8|16|8192|1024|(packet?512:64)|(packet?0:1)|(shared&&!packet?2048:0)));
        buffers[i].fd=buffers[i].handle=true;
        if(!fail || partial){put(data,88,handle(i));put(data,92,100+i);}else lost=true;
    } else if(op==0x117) {
        assert(!phase && fd==31 && get(p,4,4)==96);
        unsigned i=get(data,72,4)-100;assert(i+1==next_buffer && i%3==0 && buffers[i].fd);
        assert(get(data,0,4)==54 && get(data,64,4)==1 && get(data,68,4)==(1|16|128|8192|1024));
        buffers[i].handle=true;
        if(!fail || partial){put(data,80,duplicate && i==3?handle(0):handle(i));put(data,84,4096);}else lost=true;
    } else if(op==0x10b) {
        assert(phase==1 && fd==32 && get(p,4,4)==1456 && !submitted);verify_stream(data);submitted++;
    } else if(op==0x10c) {
        assert(integrating && phase==2 && fd==32 && get(p,4,4)==1232);
        assert(get(data,0,4)==41 && get(data,4,4)==42);
        uint64_t id=capture_index%count+1;
        if(get(data,8,4)==2) {
            assert(!held && !get(data,264,4));held=true;
            put(data,268,1);uint64_t frame[4]={id,100+capture_index*10,100+capture_index*10,capture_index};
            memcpy((unsigned char *)data+272,frame,sizeof(frame));
        } else {
            assert(get(data,8,4)==1 && held && !buffers[(id-1)*3].cpu);
            assert(get(data,16,4)==1 && get(data,24,8)==id);
            held=false;capture_returns++;
        }
    } else {
        assert(phase==3 && quiesced && fd==31 && op==0x118 && get(p,4,4)==8);
        unsigned i=handle_index(get(data,0,4));assert(buffers[i].fd && buffers[i].handle);buffers[i].handle=false;
    }
    return fail?failed():0;
}
int quest_test_fcntl(int fd,int op,...)
{ assert(!phase && fd>=100 && (unsigned)fd<100+next_buffer && buffers[fd-100].fd && op==F_GETFD);return failure()?failed():FD_CLOEXEC; }
void *quest_test_mmap(void *address,size_t size,int prot,int flags,int fd,off_t offset)
{
#ifdef QUEST_CAMERA_POOL_INSTALLED
    if(fd==-1 && (flags&MAP_ANONYMOUS))return (void *)syscall(SYS_mmap,address,size,prot,flags,fd,offset);
#endif
    assert(!phase && !address && prot==(PROT_READ|PROT_WRITE) && flags==MAP_SHARED && !offset);
    assert(fd>=100 && (unsigned)fd<100+next_buffer && buffers[fd-100].fd && size==buffers[fd-100].size);
    if(failure()){errno=EINTR;return MAP_FAILED;}
    buffers[fd-100].mapped=true;memset(buffers[fd-100].data,0xa5,sizeof(buffers[fd-100].data));return buffers[fd-100].data;
}
int quest_test_munmap(void *address,size_t size)
{
#ifdef QUEST_CAMERA_POOL_INSTALLED
    if((uintptr_t)address<(uintptr_t)buffers || (uintptr_t)address>=(uintptr_t)(buffers+90))return (int)syscall(SYS_munmap,address,size);
#endif
    assert(phase==3 && quiesced);unsigned i=0;while(i<next_buffer && address!=buffers[i].data)i++;
    assert(i<next_buffer && buffers[i].mapped && !buffers[i].handle && size==buffers[i].size);
    if(failure())return failed();
    buffers[i].mapped=false;return 0;
}
int quest_test_close(int fd)
{
    assert(phase==3 && quiesced && fd>=100 && (unsigned)fd<100+next_buffer);
    unsigned i=fd-100;assert(buffers[i].fd && !buffers[i].handle && !buffers[i].mapped);
    buffers[i].fd=false;return failure()?failed():0;
}
static struct quest_camera_pool_config config(void)
{ return (struct quest_camera_pool_config){.crm_fd=31,.isp_fd=32,.heap_fd=33,.session=41,.device=42,
    .image_mmu=54,.command_mmu=55,.command_mapping=shared?QUEST_CAMERA_MEMORY_SHARED:QUEST_CAMERA_MEMORY_IO,
    .count=count,.command_size=8192,.width=32,.height=33,.stride=64,.slice_height=33,.bits=integrating?8U:10U,.rdi=2,.pattern=5}; }
static void run(unsigned prep_fail,bool copy,unsigned configure_fail,unsigned cpu_fail,unsigned release_fail,bool cancel,bool alias)
{
    memset(buffers,0,sizeof(buffers));next_buffer=operations=phase=submitted=cache_calls=0;
    partial=copy;lost=quiesced=false;duplicate=alias;fail_at=prep_fail;
    struct quest_camera_pool_config c=config();struct quest_camera_pool *pool=NULL;
    int rc=quest_camera_pool_prepare(&c,&pool);assert(pool);
    assert(rc==(alias?-EPROTO:prep_fail?-EINTR:0));
    struct quest_camera_capture_buffer views[30],saved[30];memset(views,0xa5,sizeof(views));memcpy(saved,views,sizeof(views));
    assert(!quest_camera_pool_capture_buffers(pool,views,30) && !memcmp(views,saved,sizeof(views)));
    unsigned before=operations;
    if(prep_fail || alias)assert(quest_camera_pool_configure(pool)==-EINVAL && operations==before);
    else {
        assert(operations==count*16);memset(&c,0xa5,sizeof(c));phase=1;operations=0;fail_at=configure_fail;
        if(!cancel) {
            assert(quest_camera_pool_configure(pool)==(configure_fail?-EINTR:0) && submitted==1);
            before=operations;assert(quest_camera_pool_configure(pool)==-EINVAL && operations==before);
            if(!configure_fail) {
                assert(!quest_camera_pool_capture_buffers(pool,views,count-1) && !memcmp(views,saved,sizeof(views)));
                assert(quest_camera_pool_capture_buffers(pool,views,30));
                for(unsigned i=0;i<count;i++)assert(views[i].id==i+1 && views[i].pixels==buffers[i*3].data && views[i].size==4096);
                phase=2;operations=0;fail_at=cpu_fail;
                assert(!quest_camera_pool_cpu_sync(pool,&c,false));
                for(unsigned i=0;i<count;i++) {
                    before=operations;
                    assert(!quest_camera_pool_cpu_sync(pool,views[i].token,true) && operations==before);
                    bool start=quest_camera_pool_cpu_sync(pool,views[i].token,false);
                    if(!start){assert(cpu_fail==operations);break;}
                    before=operations;assert(!quest_camera_pool_cpu_sync(pool,views[i].token,false) && operations==before);
                    bool end=quest_camera_pool_cpu_sync(pool,views[i].token,true);
                    if(!end){assert(cpu_fail==operations);break;}
                }
                if(cpu_fail) {
                    before=operations;
                    assert(!quest_camera_pool_cpu_sync(pool,views[0].token,false) && operations==before);
                    assert(!quest_camera_pool_capture_buffers(pool,views,30));
                } else assert(cache_calls==count*2);
            }
        }
    }
    before=operations;
    if(!cancel)assert(quest_camera_pool_release(&pool)==-EBUSY && operations==before);
    if(!cancel){assert(!quest_camera_pool_quiesced(pool));assert(quest_camera_pool_quiesced(pool)==-EINVAL);}
    quiesced=true;phase=3;operations=0;fail_at=release_fail;
    rc=quest_camera_pool_release(&pool);
    if(lost || release_fail || alias) {
        assert(pool && rc==(alias?-EPROTO:lost?-EIO:-EINTR));before=operations;
        assert(quest_camera_pool_release(&pool)==rc && operations==before);
        for(unsigned i=0;i<count;i++)for(unsigned r=0;r<3;r++) {
            struct quest_camera_allocation_view view;
            if(quest_camera_pool_resources(pool,i,r,&view))assert(view.size==(r==1?8192U:4096U));
        }
        memset(buffers,0,sizeof(buffers));quest_camera_pool_closed(&pool);assert(!pool && operations==before);
    } else {
        assert(!rc && !pool);
        for(unsigned i=0;i<next_buffer;i++)assert(!buffers[i].fd && !buffers[i].handle && !buffers[i].mapped);
    }
    if(prep_fail || configure_fail || cpu_fail || release_fail || alias)faults++;else cycles++;
}

static bool exposure_window(void *p,const struct quest_camera_frame *frame,
    const struct quest_camera_metadata *metadata,int64_t now,uint64_t *minimum,uint64_t *maximum)
{
    assert(p && frame->id==capture_index%count+1 && metadata->tag==176 && now==100+capture_index*10);
    *minimum=50+capture_index*10;*maximum=*minimum+9;return true;
}
static bool exposure_center(void *p,const struct quest_camera_frame *frame,
    const struct quest_camera_metadata *metadata,const struct quest_camera_shutter *shutter,int64_t now,int64_t *center)
{
    assert(p && frame->id==capture_index%count+1 && metadata->tag==176 && shutter->timestamp_us==55+capture_index*10);
    *center=now;return true;
}
static bool captured(void *p,uint64_t generation,const struct quest_camera_capture_frame *frame,int64_t now)
{
    assert(p && generation==42 && frame->width==32 && frame->height==32 && frame->center_ns==now && held);
    assert(!buffers[(capture_index%count)*3].cpu);
    for(unsigned i=0;i<1024;i++)assert(frame->pixels[i]==(unsigned char)(i+capture_index));
    capture_frames++;return true;
}
static void invalidated(void *p,uint64_t generation){assert(p && generation==42);invalidations++;}
static void capture_integration(void)
{
    integrating=true;count=2;duplicate=false;partial=true;
    memset(buffers,0,sizeof(buffers));next_buffer=operations=phase=submitted=cache_calls=fail_at=0;
    quiesced=held=false;
    struct quest_camera_pool_config c=config();struct quest_camera_pool *pool=NULL;
    assert(!quest_camera_pool_prepare(&c,&pool));phase=1;assert(!quest_camera_pool_configure(pool));
    struct quest_camera_capture_config cfg={.fd=32,.session=41,.device=42,.generation=42,
        .camera_bit=1,.metadata_bits=8,.sensor=QUEST_CAMERA_OG01A1B,.metadata={{40,41274,38425},0},
        .layout={32,32,1,64,QUEST_CAMERA_MONO8,QUEST_CAMERA_CURVE_COPY},.metadata_size=24,.buffer_count=2,.timeout_ns=1000};
    assert(quest_camera_pool_capture_buffers(pool,cfg.buffers,30));
    const struct quest_camera_capture_ops ops={quest_camera_pool_cpu_sync,exposure_window,exposure_center,captured,invalidated};
    struct quest_camera_capture *capture=NULL;
    assert(quest_camera_capture_create(&cfg,&ops,pool,&capture));assert(quest_camera_capture_start(capture,100));
    phase=2;
    for(capture_index=0;capture_index<512;capture_index++) {
        unsigned i=capture_index%count*3;assert(!buffers[i].cpu && !held);
        const unsigned char row[]={0xc0,0x2f,0xea,0xd8,0x73,0x35,0x9f,0x7f,0x0b,0x62,0x04,0x88,
                                  0x00,0x93,0x4f,0x39,0xd9,0x64,0x74,0xc2,0x1a,0x55,0x12,0xb0};
        memcpy(buffers[i].data,row,sizeof(row));
        for(unsigned n=0;n<1024;n++)buffers[i].data[64+(n/32)*64+n%32]=(unsigned char)(n+capture_index);
        int64_t now=100+capture_index*10;
        struct quest_camera_shutter shutter={.timestamp_us=55+capture_index*10,.camera_mask=1,.type=6,.tag=176};
        if(capture_index%2)assert(quest_camera_capture_shutter(capture,42,&shutter,now));
        else {assert(quest_camera_capture_poll(capture,now)==0);assert(held);assert(quest_camera_capture_shutter(capture,42,&shutter,now));}
        assert(quest_camera_capture_poll(capture,now)==1 && !held);
        assert(capture_frames==capture_index+1 && capture_returns==capture_frames && cache_calls==capture_frames*2);
    }
    quest_camera_capture_stop(capture);assert(invalidations==1);
    assert(!quest_camera_capture_destroy(&capture));assert(quest_camera_pool_release(&pool)==-EBUSY);
    assert(quest_camera_capture_quiesced(capture));assert(quest_camera_capture_destroy(&capture));
    assert(!quest_camera_pool_quiesced(pool));quiesced=true;phase=3;
    assert(!quest_camera_pool_release(&pool) && !pool);
    integrating=false;
}
int main(void)
{
    for(shared=false;;shared=true) {
        for(count=1;count<=30;count++){run(0,true,0,0,0,false,false);run(0,true,0,0,0,true,false);}
        const unsigned counts[]={1,2,30};
        for(unsigned n=0;n<3;n++) {
            count=counts[n];
            for(unsigned i=1;i<=count*16;i++){run(i,true,0,0,0,false,false);run(i,false,0,0,0,false,false);}
            run(0,true,1,0,0,false,false);
            for(unsigned i=1;i<=count*2;i++)run(0,true,0,i,0,false,false);
            for(unsigned i=1;i<=count*9;i++)run(0,true,0,0,i,false,false);
        }
        count=2;run(0,true,0,0,0,false,true);
        if(shared)break;
    }
    unsigned before=operations;struct quest_camera_pool *pool=NULL;
    for(unsigned i=0;i<22;i++) {
        struct quest_camera_pool_config c=config();
        switch(i) {
        case 0:c.count=0;break;case 1:c.count=31;break;case 2:c.crm_fd=-1;break;case 3:c.heap_fd=c.crm_fd;break;
        case 4:c.isp_fd=c.heap_fd;break;case 5:c.session=0;break;case 6:c.device=0;break;
        case 7:c.image_mmu=-1;break;case 8:c.command_mmu=-1;break;case 9:c.command_mapping=QUEST_CAMERA_MEMORY_CPU;break;
        case 10:c.command_size=4095;break;case 11:c.command_size=1048577;break;case 12:c.width=0;break;
        case 13:c.width=31;break;case 14:c.height=0;break;case 15:c.height=4097;break;case 16:c.stride=39;break;
        case 17:c.slice_height=32;break;case 18:c.bits=12;break;case 19:c.pattern=8;break;case 20:c.rdi=4;break;
        case 21:c.stride=65536;c.slice_height=65536;break;
        }
        assert(quest_camera_pool_prepare(&c,&pool)==-EINVAL && !pool && operations==before);
    }
    assert(!quest_camera_pool_release(&pool));
    capture_integration();
    printf("QUEST_CAMERA_POOL_PASS cycles=%u faults=%u frames=%u hardware=0\n",cycles,faults,capture_frames);
}
