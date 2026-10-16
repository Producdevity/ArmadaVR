#include "quest-camera-control.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <media/cam_defs.h>
#include <media/cam_sensor.h>

static unsigned calls, mode;
static uint32_t opcode;
static int returned, failure;
static bool combo, cphy;
int ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request); struct cam_control *c=va_arg(args,void *); va_end(args);
    assert(fd==31 && request==VIDIOC_CAM_CONTROL && c->op_code==opcode && !c->reserved); ++calls;
    if(opcode==CAM_SENSOR_PROBE_CMD) {
        assert(c->size==8 && c->handle_type==CAM_HANDLE_MEM_HANDLE && c->handle==404);
    } else {
        assert(c->handle_type==CAM_HANDLE_USER_POINTER && c->handle);
        void *p=(void *)(uintptr_t)c->handle;
        if(opcode==CAM_ACQUIRE_DEV) {
            assert(c->size==sizeof(struct cam_acquire_dev_cmd));
            struct cam_acquire_dev_cmd *a=p;
            assert(a->session_handle==101 && !a->dev_handle && a->handle_type==CAM_HANDLE_USER_POINTER);
            if(mode==2) assert(a->num_resources==CAM_API_COMPAT_CONSTANT && !a->resource_hdl);
            else {
                struct cam_sensor_acquire_dev *s=p;
                assert(!s->reserved);
                if(mode==1) {
                    const struct cam_csiphy_acquire_dev_info *i=(const void *)(uintptr_t)s->info_handle;
                    assert(i && i->combo_mode==combo && i->csiphy_3phase==cphy);
                    assert(!i->cphy_dphy_combo_mode && !i->reserve);
                } else assert(!s->info_handle);
            }
            a->dev_handle=returned;
        } else if(opcode==CAM_CONFIG_DEV) {
            struct cam_config_dev_cmd *a=p;
            assert(c->size==sizeof(*a) && a->session_handle==101 && a->dev_handle==201);
            assert(a->offset==64 && a->packet_handle==404);
        } else {
            assert(opcode==CAM_START_DEV || opcode==CAM_STOP_DEV || opcode==CAM_RELEASE_DEV);
            struct cam_start_stop_dev_cmd *a=p;
            assert(c->size==sizeof(*a) && a->session_handle==101 && a->dev_handle==201);
        }
    }
    if(failure<0){errno=-failure;return -1;}return failure;
}
static int acquire(int32_t *device)
{
    return mode==0?quest_camera_sensor_acquire(31,101,device):
        mode==1?quest_camera_phy_acquire(31,101,combo,cphy,device):
                quest_camera_isp_device_acquire(31,101,device);
}
int main(void)
{
    _Static_assert(sizeof(struct cam_packet)==64,"minimum packet extent");
    const int errors[]={0,-EINTR,-EFAULT,-EIO,-ENODEV,1};
    const int handles[]={0,-1,201,INT32_MAX};
    for(mode=0;mode<3;mode++) for(unsigned variant=0;variant<4;variant++) {
        combo=(variant&1)!=0;cphy=(variant&2)!=0;
        for(unsigned e=0;e<sizeof(errors)/sizeof(*errors);e++) for(unsigned h=0;h<4;h++) {
            failure=errors[e];returned=handles[h];int32_t device=0;opcode=CAM_ACQUIRE_DEV;
            int expected=failure?(failure<0?failure:-EPROTO):returned>0?0:-EPROTO;
            unsigned before=calls;assert(acquire(&device)==expected && calls==before+1);
            assert(device==(returned>0?returned:0));
        }
    }
    for(unsigned e=0;e<sizeof(errors)/sizeof(*errors);e++) {
        failure=errors[e];int expected=failure>0?-EPROTO:failure;int32_t device=201;
        opcode=CAM_SENSOR_PROBE_CMD;assert(quest_camera_sensor_probe_submit(31,404)==expected);
        opcode=CAM_CONFIG_DEV;assert(quest_camera_device_configure(31,101,201,404,64,128)==expected);
        opcode=CAM_START_DEV;assert(quest_camera_device_stream(31,101,201,true)==expected);
        opcode=CAM_STOP_DEV;assert(quest_camera_device_stream(31,101,201,false)==expected);
        opcode=CAM_RELEASE_DEV;assert(quest_camera_device_release(31,101,&device)==expected);
        assert(device==(expected?201:0));
    }
    unsigned before=calls;int32_t device=0;
    assert(quest_camera_sensor_acquire(-1,101,&device)==-EINVAL);
    assert(quest_camera_sensor_acquire(31,0,&device)==-EINVAL);
    assert(quest_camera_phy_acquire(31,101,false,false,NULL)==-EINVAL);
    device=201;assert(quest_camera_isp_device_acquire(31,101,&device)==-EINVAL && device==201);
    assert(quest_camera_sensor_probe_submit(31,0)==-EINVAL);
    assert(quest_camera_device_configure(31,101,201,404,64,127)==-EINVAL);
    assert(quest_camera_device_configure(31,101,201,404,0,63)==-EINVAL);
    assert(quest_camera_device_configure(31,101,201,404,65,256)==-EINVAL);
    assert(quest_camera_device_configure(31,101,201,404,0,UINT64_MAX)==-EINVAL);
    assert(quest_camera_device_stream(31,0,201,true)==-EINVAL);
    assert(quest_camera_device_release(31,101,NULL)==-EINVAL);
    assert(calls==before);
    printf("QUEST_CAMERA_ACQUIRE_PASS ioctls=%u hardware=0\n",calls);
}
