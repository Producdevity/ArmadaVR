#include "quest-camera-buffer.h"
#include <sys/types.h>
#include <media/cam_defs.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned calls,mode,count;
static int failure;
static struct cam_set_stream_mode_frame_params response={UINT64_MAX,123456789,123999999,42};
static uint32_t response_count=1;
static void zero(const void *p,size_t n){const unsigned char *b=p;for(size_t i=0;i<n;i++)assert(!b[i]);}
int ioctl(int fd,unsigned long request,...)
{
    assert(fd==51 && request==VIDIOC_CAM_CONTROL);calls++;
    va_list ap;va_start(ap,request);struct cam_control *c=va_arg(ap,void *);va_end(ap);
    assert(c->handle_type==CAM_HANDLE_USER_POINTER && !c->reserved && c->handle);
    if(!mode) {
        struct cam_set_stream_mode *v=(void *)(uintptr_t)c->handle;
        assert(c->op_code==CAM_SET_STREAM_MODE && c->size==sizeof(*v));
        assert(v->session_handle==17 && v->dev_handle==23 && v->num_images==count);
        for(unsigned i=0;i<30;i++) {
            if(i>=count){zero(&v->stream_images[i],sizeof(v->stream_images[i]));continue;}
            struct cam_stream_image *image=&v->stream_images[i];
            assert(image->image_id==UINT64_MAX-i && image->mem_handles[0]==100+i);
            assert(!image->mem_handles[1] && !image->mem_handles[2] && image->packet_handle==200+i && image->packet_offset==8*i);
        }
    } else {
        struct cam_stream_mode_cmd *v=(void *)(uintptr_t)c->handle;
        assert(c->op_code==CAM_STREAM_MODE_CMD && c->size==sizeof(*v));
        assert(v->session_handle==17 && v->dev_handle==23 && v->cmd_mode==mode);
        zero(&v->cmd_get,sizeof(v->cmd_get));
        if(mode==CAM_STREAM_MODE_GET_IMAGE) {
            zero(&v->cmd_return,sizeof(v->cmd_return));
            v->cmd_get.num_images=response_count;v->cmd_get.images[0]=response;
        } else {
            assert(mode==CAM_STREAM_MODE_RETURN_IMAGE && v->cmd_return.num_images==1 && v->cmd_return.image_ids[0]==UINT64_MAX);
            zero(&v->cmd_return.image_ids[1],29*sizeof(uint64_t));
        }
    }
    if(failure>0){errno=failure;return -1;}return failure?1:0;
}
int main(void)
{
    _Static_assert(CAM_MAX_STREAM_MODE_HANDLES==30 && CAM_PACKET_MAX_PLANES==3,"Quest buffer limits");
    _Static_assert(sizeof(struct cam_set_stream_mode)==1456 && sizeof(struct cam_stream_mode_cmd)==1232,"Quest stream ABI");
    struct quest_camera_buffer buffers[30];
    for(unsigned i=0;i<30;i++)buffers[i]=(struct quest_camera_buffer){UINT64_MAX-i,100+(int)i,200+(int)i,8*i,4096};
    int errors[]={0,EIO,EFAULT,EINTR,ENODEV,ETIMEDOUT,ENOENT,-1};
    struct quest_camera_frame frame,old;
    for(unsigned e=0;e<sizeof(errors)/sizeof(errors[0]);e++) {
        failure=errors[e];int want=failure>0?-failure:failure?-EPROTO:0;
        mode=0;
        for(count=1;count<=30;count++)assert(quest_camera_buffers_configure(51,17,23,buffers,count)==want);
        mode=CAM_STREAM_MODE_GET_IMAGE;memset(&frame,0xa5,sizeof(frame));old=frame;
        assert(quest_camera_buffer_get(51,17,23,&frame)==want);
        if(want)assert(!memcmp(&old,&frame,sizeof(frame)));
        else assert(frame.id==UINT64_MAX && frame.qtimer_ns==123456789 && frame.observed_monotonic_ns==123999999 && frame.frame_number==42);
        mode=CAM_STREAM_MODE_RETURN_IMAGE;assert(quest_camera_buffer_return(51,17,23,UINT64_MAX)==want);
    }
    failure=0;mode=CAM_STREAM_MODE_GET_IMAGE;old=frame;
    uint32_t counts[]={0,2,30,31,UINT32_MAX};
    for(unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);i++){
        response_count=counts[i];assert(quest_camera_buffer_get(51,17,23,&frame)==-EPROTO && !memcmp(&frame,&old,sizeof(frame)));
    }
    response_count=1;struct cam_set_stream_mode_frame_params valid=response;
#define BAD_REPLY(expr) do{response=valid;expr;assert(quest_camera_buffer_get(51,17,23,&frame)==-EPROTO && !memcmp(&frame,&old,sizeof(frame)));}while(0)
    BAD_REPLY(response.image_id=0);BAD_REPLY(response.timestamp=0);BAD_REPLY(response.timestamp=UINT64_MAX);
    BAD_REPLY(response.sof_timestamp=UINT64_MAX);BAD_REPLY(response.frame_num=-1);
    unsigned before=calls;
#define BAD(v) assert((v)==-EINVAL)
    BAD(quest_camera_buffers_configure(-1,17,23,buffers,2));BAD(quest_camera_buffers_configure(51,0,23,buffers,2));
    BAD(quest_camera_buffers_configure(51,17,0,buffers,2));BAD(quest_camera_buffers_configure(51,17,23,NULL,2));
    BAD(quest_camera_buffers_configure(51,17,23,buffers,0));BAD(quest_camera_buffers_configure(51,17,23,buffers,31));
    struct quest_camera_buffer original=buffers[1];
#define BAD_BUFFER(expr) do{buffers[1]=original;expr;BAD(quest_camera_buffers_configure(51,17,23,buffers,2));}while(0)
    BAD_BUFFER(buffers[1].id=0);BAD_BUFFER(buffers[1].id=buffers[0].id);
    BAD_BUFFER(buffers[1].pixels=0);BAD_BUFFER(buffers[1].pixels=buffers[0].pixels);
    BAD_BUFFER(buffers[1].packet=-1);BAD_BUFFER(buffers[1].packet=buffers[0].packet);
    BAD_BUFFER(buffers[1].packet_offset=1);BAD_BUFFER(buffers[1].packet_offset=4040);
    BAD_BUFFER(buffers[1].packet_allocation_size=56);BAD_BUFFER(buffers[1].packet_allocation_size=UINT64_MAX);
    BAD(quest_camera_buffer_get(-1,17,23,&frame));BAD(quest_camera_buffer_get(51,0,23,&frame));
    BAD(quest_camera_buffer_get(51,17,0,&frame));BAD(quest_camera_buffer_get(51,17,23,NULL));
    BAD(quest_camera_buffer_return(-1,17,23,1));BAD(quest_camera_buffer_return(51,0,23,1));
    BAD(quest_camera_buffer_return(51,17,0,1));BAD(quest_camera_buffer_return(51,17,23,0));
    assert(calls==before && calls==266);
    printf("QUEST_CAMERA_BUFFER_PASS ioctls=%u nonblocking=1 failure_output_preserved=1 hardware=0\n",calls);
}
