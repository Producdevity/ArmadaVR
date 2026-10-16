#include "quest-camera-memory.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <media/cam_defs.h>
#include <media/cam_req_mgr.h>

static unsigned calls, count, malformed;
static uint32_t opcode, flags, extent;
static int dma_fd, failure, output_fd;
static uint32_t output_handle;
static void mmus(const int32_t *values)
{
    for(unsigned i=0;i<CAM_MEM_MMU_MAX_HANDLE;i++) assert(values[i]==(i<count?(int32_t)(100+i):0));
}
int ioctl(int fd, unsigned long request, ...)
{
    va_list args;va_start(args,request);struct cam_control *c=va_arg(args,void *);va_end(args);
    assert(fd==31 && request==VIDIOC_CAM_CONTROL && c->op_code==opcode);
    assert(c->handle_type==CAM_HANDLE_USER_POINTER && !c->reserved && c->handle);++calls;
    void *data=(void *)(uintptr_t)c->handle;
    output_handle=GET_MEM_HANDLE(7,dma_fd);output_fd=dma_fd;
    if(malformed==1)output_handle=0;
    if(malformed==2)output_handle=GET_MEM_HANDLE(0,dma_fd);
    if(malformed==3)output_handle|=1<<CAM_MEM_MGR_SECURE_BIT_POS;
    if(malformed==4)output_handle=GET_MEM_HANDLE(7,(dma_fd==0?1:0));
    if(malformed==6)output_fd=-1;
    if(malformed==7)output_fd=32768;
    if(opcode==CAM_REQ_MGR_ALLOC_BUF) {
        struct cam_mem_mgr_alloc_cmd *a=data;
        assert(c->size==sizeof(*a) && a->len==extent && a->align==4096);
        assert(a->flags==flags && a->num_hdl==count);mmus(a->mmu_hdls);
        assert(!a->out.buf_handle && a->out.fd==-1 && !a->out.vaddr);
        if(malformed==8){output_handle=0x80000007u;output_fd=32768;}
        a->out.buf_handle=output_handle;a->out.fd=output_fd;a->out.vaddr=malformed==5;
    } else if(opcode==CAM_REQ_MGR_MAP_BUF) {
        struct cam_mem_mgr_map_cmd *a=data;
        assert(c->size==sizeof(*a) && a->fd==dma_fd && !a->reserved);
        assert(a->flags==flags && a->num_hdl==count);mmus(a->mmu_hdls);
        assert(!a->out.buf_handle && !a->out.size && !a->out.vaddr);
        if(malformed==9)output_handle=0x80000007u;
        a->out.buf_handle=output_handle;a->out.vaddr=malformed==5;
        a->out.size=malformed==6?0:malformed==7?extent-1:malformed==8?extent+4096:extent;
    } else {
        struct cam_mem_mgr_release_cmd *a=data;
        assert(opcode==CAM_REQ_MGR_RELEASE_BUF && c->size==sizeof(*a));
        assert(a->buf_handle==(int32_t)GET_MEM_HANDLE(7,dma_fd) && !a->reserved);
    }
    if(failure<0){errno=-failure;return -1;}return failure;
}
int main(void)
{
    int32_t handles[17];for(unsigned i=0;i<17;i++)handles[i]=100+i;
    const int errors[]={0,-EINTR,-EFAULT,-ENOMEM,-ENODEV,1};
    dma_fd=41;
    for(unsigned variant=0;variant<4;variant++) {
        enum quest_camera_memory_kind kind=variant?QUEST_CAMERA_MEMORY_COMMAND:QUEST_CAMERA_MEMORY_PACKET;
        enum quest_camera_memory_mapping mapping=variant<2?QUEST_CAMERA_MEMORY_CPU:
            variant==2?QUEST_CAMERA_MEMORY_IO:QUEST_CAMERA_MEMORY_SHARED;
        for(count=variant<2?0:1;count<=(variant<2?0:16);count++) for(unsigned big=0;big<2;big++) {
            extent=big?0x7ffff000:4096;flags=CAM_MEM_FLAG_KMD_ACCESS|CAM_MEM_FLAG_UMD_ACCESS|
                CAM_MEM_FLAG_DISABLE_DELAYED_UNMAP|CAM_MEM_FLAG_CACHE|
                (variant?CAM_MEM_FLAG_CMD_BUF_TYPE:CAM_MEM_FLAG_PACKET_BUF_TYPE)|
                (count?CAM_MEM_FLAG_HW_READ_WRITE:0)|(variant==3?CAM_MEM_FLAG_HW_SHARED_ACCESS:0);
            for(unsigned e=0;e<6;e++) for(malformed=0;malformed<9;malformed++) {
                failure=errors[e];opcode=CAM_REQ_MGR_ALLOC_BUF;
                struct quest_camera_memory_allocation a={.fd=-1};
                unsigned before=calls;int expected=failure?(failure<0?failure:-EPROTO):malformed?-EPROTO:0;
                assert(quest_camera_memory_allocate(31,kind,mapping,extent,handles,count,&a)==expected && calls==before+1);
                assert(a.handle==(int32_t)output_handle && a.fd==output_fd);
            }
        }
    }
    const int fds[]={0,41,32767};
    for(unsigned f=0;f<3;f++) for(count=1;count<=16;count++) {
        dma_fd=fds[f];extent=4096;flags=CAM_MEM_FLAG_HW_READ_WRITE|CAM_MEM_FLAG_UMD_ACCESS|
            CAM_MEM_FLAG_PIXEL_BUF_TYPE|CAM_MEM_FLAG_DISABLE_DELAYED_UNMAP|CAM_MEM_FLAG_CACHE;
        for(unsigned e=0;e<6;e++) for(malformed=0;malformed<10;malformed++) {
            failure=errors[e];opcode=CAM_REQ_MGR_MAP_BUF;int32_t handle=0;
            int expected=failure?(failure<0?failure:-EPROTO):malformed?-EPROTO:0;
            unsigned before=calls;
            int result=quest_camera_pixels_import(31,dma_fd,handles,count,extent,&handle);
            if(result!=expected) fprintf(stderr,"fd=%d mmus=%u malformed=%u failure=%d result=%d expected=%d\n",dma_fd,count,malformed,failure,result,expected);
            assert(result==expected && calls==before+1);
            assert(handle==(int32_t)output_handle);
        }
    }
    malformed=0;dma_fd=41;
    for(unsigned e=0;e<6;e++) {
        failure=errors[e];opcode=CAM_REQ_MGR_RELEASE_BUF;int32_t handle=GET_MEM_HANDLE(7,dma_fd);
        int expected=failure>0?-EPROTO:failure;
        assert(quest_camera_memory_release(31,&handle)==expected);
        assert(handle==(expected?(int32_t)GET_MEM_HANDLE(7,dma_fd):0));
    }
    unsigned before=calls;struct quest_camera_memory_allocation a={.fd=-1};int32_t handle=0;
    assert(quest_camera_memory_allocate(-1,QUEST_CAMERA_MEMORY_PACKET,QUEST_CAMERA_MEMORY_CPU,4096,NULL,0,&a)==-EINVAL);
    assert(quest_camera_memory_allocate(31,QUEST_CAMERA_MEMORY_PACKET,QUEST_CAMERA_MEMORY_CPU,0,NULL,0,&a)==-EINVAL);
    assert(quest_camera_memory_allocate(31,QUEST_CAMERA_MEMORY_PACKET,QUEST_CAMERA_MEMORY_CPU,4095,NULL,0,&a)==-EINVAL);
    assert(quest_camera_memory_allocate(31,QUEST_CAMERA_MEMORY_PACKET,QUEST_CAMERA_MEMORY_CPU,0x80000000,NULL,0,&a)==-EINVAL);
    assert(quest_camera_memory_allocate(31,QUEST_CAMERA_MEMORY_PACKET,QUEST_CAMERA_MEMORY_IO,4096,handles,1,&a)==-EINVAL);
    assert(quest_camera_memory_allocate(31,QUEST_CAMERA_MEMORY_COMMAND,QUEST_CAMERA_MEMORY_IO,4096,NULL,0,&a)==-EINVAL);
    assert(quest_camera_memory_allocate(31,QUEST_CAMERA_MEMORY_COMMAND,QUEST_CAMERA_MEMORY_CPU,4096,handles,1,&a)==-EINVAL);
    a.fd=0;assert(quest_camera_memory_allocate(31,QUEST_CAMERA_MEMORY_PACKET,QUEST_CAMERA_MEMORY_CPU,4096,NULL,0,&a)==-EINVAL);
    assert(quest_camera_pixels_import(31,41,handles,17,4096,&handle)==-EINVAL);
    assert(quest_camera_pixels_import(31,32768,handles,1,4096,&handle)==-EINVAL);
    assert(quest_camera_pixels_import(31,-1,handles,1,4096,&handle)==-EINVAL);
    assert(quest_camera_pixels_import(31,41,handles,0,4096,&handle)==-EINVAL);
    assert(quest_camera_pixels_import(31,41,handles,1,4095,&handle)==-EINVAL);
    assert(quest_camera_pixels_import(31,41,handles,1,UINT32_MAX,&handle)==-EINVAL);
    handles[1]=handles[0];assert(quest_camera_pixels_import(31,41,handles,2,4096,&handle)==-EINVAL);
    handles[0]=-1;assert(quest_camera_pixels_import(31,41,handles,1,4096,&handle)==-EINVAL);
    assert(quest_camera_memory_release(31,&handle)==-EINVAL);
    assert(calls==before);
    printf("QUEST_CAMERA_MEMORY_PASS ioctls=%u hardware=0\n",calls);
}
