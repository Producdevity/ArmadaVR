#include "quest-camera-pool.h"
#include "quest-camera-packet.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct pool_image { struct quest_camera_allocation *resources[QUEST_CAMERA_POOL_RESOURCES]; };
struct quest_camera_pool {
    struct quest_camera_pool_config config;
    struct pool_image images[30];
    struct pool_image *held;
    bool prepared, attempted, configured, failed, ambiguous, quiesced, releasing;
    int release_error;
};
static bool unique_resources(const struct quest_camera_pool *p)
{
    struct quest_camera_allocation_view views[90];size_t count=0;
    for(size_t i=0;i<p->config.count;i++)for(unsigned r=0;r<QUEST_CAMERA_POOL_RESOURCES;r++) {
        struct quest_camera_allocation_view view;
        if(!quest_camera_allocation_resources(p->images[i].resources[r],&view))continue;
        for(size_t j=0;j<count;j++)if(
            (view.handle && view.handle==views[j].handle) ||
            (view.fd>=0 && view.fd==views[j].fd) ||
            (view.mapped && views[j].mapped && view.data==views[j].data))return false;
        views[count++]=view;
    }
    return true;
}
static int prepare_image(struct quest_camera_pool *p,size_t index,size_t pixel_size)
{
    struct pool_image *image=&p->images[index];
    struct quest_camera_allocation_view views[QUEST_CAMERA_POOL_RESOURCES];
    for(unsigned r=0;r<QUEST_CAMERA_POOL_RESOURCES;r++) {
        struct quest_camera_allocation_config c={.kind=r==QUEST_CAMERA_POOL_PIXELS?QUEST_CAMERA_ALLOCATION_PIXELS:
            r==QUEST_CAMERA_POOL_COMMAND?QUEST_CAMERA_ALLOCATION_COMMAND:QUEST_CAMERA_ALLOCATION_PACKET,
            .mapping=r==QUEST_CAMERA_POOL_PIXELS?QUEST_CAMERA_MEMORY_IO:
                r==QUEST_CAMERA_POOL_COMMAND?p->config.command_mapping:QUEST_CAMERA_MEMORY_CPU,
            .size=r==QUEST_CAMERA_POOL_PIXELS?pixel_size:r==QUEST_CAMERA_POOL_COMMAND?p->config.command_size:4096,
            .mmus={r==QUEST_CAMERA_POOL_PIXELS?p->config.image_mmu:p->config.command_mmu},
            .mmu_count=r==QUEST_CAMERA_POOL_PACKET?0:1};
        int result=quest_camera_allocation_create(p->config.crm_fd,p->config.heap_fd,&c,&image->resources[r]);
        if(!unique_resources(p)){p->ambiguous=true;return -EPROTO;}
        if(result)return result;
        if(!quest_camera_allocation_view(image->resources[r],&views[r]))return -EINVAL;
        result=quest_camera_allocation_cpu_begin(image->resources[r],QUEST_CAMERA_CPU_WRITE);
        if(result)return result;
        memset(views[r].data,0,views[r].size);
        bool encoded=true;
        if(r==QUEST_CAMERA_POOL_PACKET) {
            struct quest_camera_command_buffer command={.memory_handle=views[QUEST_CAMERA_POOL_COMMAND].handle,
                .capacity=(uint32_t)views[QUEST_CAMERA_POOL_COMMAND].size,.allocation_size=views[QUEST_CAMERA_POOL_COMMAND].size};
            struct quest_camera_rdi_buffer pixels={.memory_handle=views[QUEST_CAMERA_POOL_PIXELS].handle,
                .width=p->config.width,.height=p->config.height,.stride=p->config.stride,.slice_height=p->config.slice_height,
                .bits=p->config.bits,.rdi=p->config.rdi,.format=QUEST_CAMERA_RAW_MIPI,.allocation_size=pixel_size};
            encoded=quest_camera_stream_packet_encode(&command,&pixels,p->config.pattern,views[r].data,views[r].size)!=0;
        }
        result=quest_camera_allocation_cpu_end(image->resources[r]);
        if(result || !encoded)return result?result:-EINVAL;
    }
    return 0;
}
int quest_camera_pool_prepare(const struct quest_camera_pool_config *c,struct quest_camera_pool **pool)
{
    if(!c || !pool || *pool || !c->count || c->count>30 || c->crm_fd<0 || c->heap_fd<0 || c->isp_fd<0 ||
        c->crm_fd==c->heap_fd || c->crm_fd==c->isp_fd || c->isp_fd==c->heap_fd ||
        c->session<=0 || c->device<=0 || c->image_mmu<0 || c->command_mmu<0 ||
        (c->command_mapping!=QUEST_CAMERA_MEMORY_IO && c->command_mapping!=QUEST_CAMERA_MEMORY_SHARED) ||
        c->command_size<4096 || c->command_size>1048576 || c->command_size%4096)return -EINVAL;
    uint64_t extent=(uint64_t)c->stride*c->slice_height;
    uint64_t rounded=(extent+4095)&~UINT64_C(4095);
    if(!rounded || rounded>INT32_MAX)return -EINVAL;
    struct quest_camera_command_buffer command={.memory_handle=1,.capacity=(uint32_t)c->command_size,.allocation_size=c->command_size};
    struct quest_camera_rdi_buffer image={.memory_handle=2,.width=c->width,.height=c->height,.stride=c->stride,
        .slice_height=c->slice_height,.bits=c->bits,.rdi=c->rdi,.format=QUEST_CAMERA_RAW_MIPI,.allocation_size=rounded};
    uint8_t packet[QUEST_CAMERA_STREAM_PACKET_SIZE];
    if(!quest_camera_stream_packet_encode(&command,&image,c->pattern,packet,sizeof(packet)))return -EINVAL;
    struct quest_camera_pool *p=calloc(1,sizeof(*p));if(!p)return -ENOMEM;
    *pool=p;p->config=*c;
    for(size_t i=0;i<c->count;i++) {
        int result=prepare_image(p,i,(size_t)rounded);
        if(result){p->failed=true;return result;}
    }
    p->prepared=true;return 0;
}
int quest_camera_pool_configure(struct quest_camera_pool *p)
{
    if(!p || !p->prepared || p->attempted || p->failed || p->quiesced || p->releasing)return -EINVAL;
    p->attempted=true;
    struct quest_camera_buffer images[30];
    for(size_t i=0;i<p->config.count;i++) {
        struct quest_camera_allocation_view views[QUEST_CAMERA_POOL_RESOURCES];
        for(unsigned r=0;r<QUEST_CAMERA_POOL_RESOURCES;r++) {
            int result=quest_camera_allocation_submit(p->images[i].resources[r]);
            if(result){p->failed=true;return result;}
            if(!quest_camera_allocation_view(p->images[i].resources[r],&views[r])){p->failed=true;return -EINVAL;}
        }
        images[i]=(struct quest_camera_buffer){.id=i+1,.pixels=views[QUEST_CAMERA_POOL_PIXELS].handle,
            .packet=views[QUEST_CAMERA_POOL_PACKET].handle,.packet_allocation_size=views[QUEST_CAMERA_POOL_PACKET].size};
    }
    int result=quest_camera_buffers_configure(p->config.isp_fd,p->config.session,p->config.device,images,p->config.count);
    p->failed=result!=0;p->configured=!p->failed;return result;
}
bool quest_camera_pool_capture_buffers(struct quest_camera_pool *p,struct quest_camera_capture_buffer *buffers,size_t count)
{
    if(!p || !buffers || count<p->config.count || !p->configured || p->failed || p->quiesced || p->releasing)return false;
    struct quest_camera_capture_buffer views[30];
    for(size_t i=0;i<p->config.count;i++) {
        struct quest_camera_allocation_view pixels;
        if(!quest_camera_allocation_view(p->images[i].resources[QUEST_CAMERA_POOL_PIXELS],&pixels))return false;
        views[i]=(struct quest_camera_capture_buffer){.id=i+1,.token=&p->images[i],.pixels=pixels.data,.size=pixels.size};
    }
    memcpy(buffers,views,p->config.count*sizeof(*buffers));return true;
}
bool quest_camera_pool_cpu_sync(void *opaque,void *token,bool end)
{
    struct quest_camera_pool *p=opaque;
    if(!p || !p->configured || p->failed || p->quiesced || p->releasing)return false;
    struct pool_image *image=NULL;
    for(size_t i=0;i<p->config.count;i++)if(token==&p->images[i]){image=&p->images[i];break;}
    if(!image || (end?p->held!=image:p->held!=NULL))return false;
    struct quest_camera_allocation *pixels=image->resources[QUEST_CAMERA_POOL_PIXELS];
    int result;
    if(end) {
        result=quest_camera_allocation_cpu_end(pixels);
        if(!result)result=quest_camera_allocation_submit(pixels);
        if(!result)p->held=NULL;
    } else {
        p->held=image;
        result=quest_camera_allocation_complete(pixels);
        if(!result)result=quest_camera_allocation_cpu_begin(pixels,QUEST_CAMERA_CPU_READ);
    }
    if(result)p->failed=true;
    return result==0;
}
int quest_camera_pool_quiesced(struct quest_camera_pool *p)
{
    if(!p || p->quiesced || p->releasing)return -EINVAL;
    for(size_t i=0;i<p->config.count;i++)for(unsigned r=0;r<QUEST_CAMERA_POOL_RESOURCES;r++)if(p->images[i].resources[r]) {
        int result=quest_camera_allocation_quiesced(p->images[i].resources[r]);if(result)return result;
    }
    p->quiesced=true;p->prepared=p->configured=false;p->held=NULL;return 0;
}
int quest_camera_pool_release(struct quest_camera_pool **pool)
{
    if(!pool)return -EINVAL;
    struct quest_camera_pool *p=*pool;if(!p)return 0;
    if((p->attempted || p->failed) && !p->quiesced)return -EBUSY;
    if(p->releasing)return p->release_error;
    p->releasing=true;p->prepared=p->configured=false;
    if(p->ambiguous)return p->release_error=-EPROTO;
    for(size_t i=0;i<p->config.count;i++)for(unsigned r=0;r<QUEST_CAMERA_POOL_RESOURCES;r++) {
        int result=quest_camera_allocation_release(&p->images[i].resources[r]);
        if(result && !p->release_error)p->release_error=result;
    }
    if(p->release_error)return p->release_error;
    free(p);*pool=NULL;return 0;
}
bool quest_camera_pool_resources(const struct quest_camera_pool *p,size_t index,
    enum quest_camera_pool_resource r,struct quest_camera_allocation_view *view)
{
    return p && index<p->config.count && r>=0 && r<QUEST_CAMERA_POOL_RESOURCES &&
        quest_camera_allocation_resources(p->images[index].resources[r],view);
}
void quest_camera_pool_closed(struct quest_camera_pool **pool)
{
    if(!pool || !*pool)return;
    struct quest_camera_pool *p=*pool;
    for(size_t i=0;i<p->config.count;i++)for(unsigned r=0;r<QUEST_CAMERA_POOL_RESOURCES;r++)
        quest_camera_allocation_closed(&p->images[i].resources[r]);
    free(p);*pool=NULL;
}
