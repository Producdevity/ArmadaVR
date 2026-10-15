#include "quest-camera-buffer.h"
#include <errno.h>
#include <sys/ioctl.h>

struct stream_image { uint64_t planes[3], id, packet, offset; };
struct stream_mode { int32_t session, device; uint32_t count, padding; struct stream_image images[30]; };
struct stream_return { uint32_t count, padding; uint64_t ids[30]; };
struct stream_frame { uint64_t id, timestamp, sof; int64_t number; };
struct stream_get { uint32_t timeout_ms, count; struct stream_frame images[30]; };
struct stream_command {
    int32_t session, device;
    uint32_t mode, padding;
    struct stream_return returned;
    struct stream_get get;
};
_Static_assert(sizeof(void *)==8 && sizeof(struct stream_mode)==1456, "Quest stream setup ABI");
_Static_assert(sizeof(struct stream_command)==1232, "Quest stream command ABI");

static int submit(int fd, uint32_t opcode, void *data, uint32_t size)
{
    if(fd<0) return -EINVAL;
    struct { uint32_t opcode, size, type, reserved; uint64_t pointer; } command={opcode,size,1,0,(uintptr_t)data};
    _Static_assert(sizeof(command)==24,"Quest camera control ABI");
    int rc=ioctl(fd,0xc01856c0UL,&command);
    return rc<0?-errno:rc?-EPROTO:0;
}
int quest_camera_buffers_configure(int fd, int32_t session, int32_t device,
                                   const struct quest_camera_buffer *buffers, size_t count)
{
    if(session<=0 || device<=0 || !buffers || !count || count>30) return -EINVAL;
    struct stream_mode data={.session=session,.device=device,.count=(uint32_t)count};
    for(size_t i=0;i<count;i++) {
        const struct quest_camera_buffer *b=&buffers[i];
        if(!b->id || b->pixels<=0 || b->packet<=0 || b->packet_offset%8 ||
           b->packet_allocation_size<=56 || b->packet_allocation_size>UINT32_MAX ||
           b->packet_offset>=b->packet_allocation_size-56) return -EINVAL;
        for(size_t j=0;j<i;j++)
            if(buffers[j].id==b->id || buffers[j].pixels==b->pixels || buffers[j].packet==b->packet) return -EINVAL;
        data.images[i]=(struct stream_image){{(uint32_t)b->pixels,0,0},b->id,(uint32_t)b->packet,b->packet_offset};
    }
    return submit(fd,0x10b,&data,sizeof(data));
}
int quest_camera_buffer_get(int fd, int32_t session, int32_t device, struct quest_camera_frame *frame)
{
    if(session<=0 || device<=0 || !frame) return -EINVAL;
    struct stream_command data={.session=session,.device=device,.mode=2};
    int rc=submit(fd,0x10c,&data,sizeof(data));
    if(rc) return rc;
    const struct stream_frame *v=&data.get.images[0];
    if(data.get.count!=1 || !v->id || !v->timestamp || v->timestamp>INT64_MAX ||
       v->sof>INT64_MAX || v->number<0) return -EPROTO;
    *frame=(struct quest_camera_frame){v->id,(int64_t)v->timestamp,(int64_t)v->sof,v->number};
    return 0;
}
int quest_camera_buffer_return(int fd, int32_t session, int32_t device, uint64_t id)
{
    if(session<=0 || device<=0 || !id) return -EINVAL;
    struct stream_command data={.session=session,.device=device,.mode=1,.returned={.count=1}};
    data.returned.ids[0]=id;
    return submit(fd,0x10c,&data,sizeof(data));
}
