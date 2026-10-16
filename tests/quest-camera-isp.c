#include "quest-camera-isp.h"
#include "quest-camera-control.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <media/cam_defs.h>
#include <media/cam_isp.h>
#include <media/cam_isp_ife.h>
#include <media/cam_sensor.h>

static unsigned cases,calls;
static int failure;
static bool release_hw;
static uint8_t resource[QUEST_CAMERA_ISP_ACQUIRE_SIZE];
static void check(struct quest_camera_isp_config *c)
{
    uint8_t actual[QUEST_CAMERA_ISP_ACQUIRE_SIZE+2],expected[QUEST_CAMERA_ISP_ACQUIRE_SIZE]={0};
    memset(actual,0xa5,sizeof(actual));
    assert(quest_camera_isp_encode(c,actual+1,sizeof(actual)-2));
    struct cam_isp_acquire_hw_info h={.common_info_version=0x2000,.num_inputs=1,
        .input_info_version=0x200,.input_info_size=sizeof(struct cam_isp_in_port_info_v2)};
    struct cam_isp_in_port_info_v2 in={.res_type=c->phy==6?CAM_ISP_IFE_IN_RES_PHY_6:CAM_ISP_IFE_IN_RES_PHY_0+c->phy,
        .lane_type=c->cphy,.lane_num=c->lanes,.lane_cfg=c->lane_map,.vc={c->virtual_channel},
        .dt={c->bits==8?0x2a:0x2b},.num_valid_vc_dt=1,.format=c->bits==8?CAM_FORMAT_MIPI_RAW_8:CAM_FORMAT_MIPI_RAW_10,
        .left_stop=c->width-1,.left_width=c->width,.line_stop=c->height-1,.height=c->height,.num_out_res=1};
    in.data[0]=(struct cam_isp_out_port_info_v2){.res_type=CAM_ISP_IFE_OUT_RES_RDI_0+c->rdi,
        .width=c->width,.height=c->height,.format=c->output==QUEST_CAMERA_RAW_MIPI?in.format:
            c->output==QUEST_CAMERA_RAW_PLAIN8?CAM_FORMAT_PLAIN8:c->bits==8?CAM_FORMAT_PLAIN16_8:CAM_FORMAT_PLAIN16_10};
    memcpy(expected,&h,offsetof(struct cam_isp_acquire_hw_info,data));
    memcpy(expected+offsetof(struct cam_isp_acquire_hw_info,data),&in,sizeof(in));
    assert(!memcmp(actual+1,expected,sizeof(expected)) && actual[0]==0xa5 && actual[sizeof(actual)-1]==0xa5);
    memcpy(resource,expected,sizeof(resource));cases++;
}
int ioctl(int fd, unsigned long request, ...)
{
    va_list args;va_start(args,request);struct cam_control *c=va_arg(args,void *);va_end(args);
    assert(fd==31 && request==VIDIOC_CAM_CONTROL && c->handle_type==CAM_HANDLE_USER_POINTER && !c->reserved);calls++;
    if(release_hw) {
        struct cam_release_hw_cmd_v1 *a=(void *)(uintptr_t)c->handle;
        assert(c->op_code==CAM_RELEASE_HW && c->size==sizeof(*a));
        assert(a->struct_version==CAM_RELEASE_HW_STRUCT_VERSION_1 && !a->reserved && a->session_handle==101 && a->dev_handle==201);
    } else {
        struct cam_acquire_hw_cmd_v2 *a=(void *)(uintptr_t)c->handle;
        assert(c->op_code==CAM_ACQUIRE_HW && c->size==sizeof(*a));
        assert(a->struct_version==CAM_ACQUIRE_HW_STRUCT_VERSION_2 && !a->reserved && a->session_handle==101 && a->dev_handle==201);
        assert(a->handle_type==CAM_HANDLE_USER_POINTER && a->data_size==sizeof(resource));
        assert(!memcmp((void *)(uintptr_t)a->resource_hdl,resource,sizeof(resource)));
        struct cam_acquired_hw_info zero={0};assert(!memcmp(&a->hw_info,&zero,sizeof(zero)));
        for(unsigned i=0;i<CAM_MAX_ACQ_RES;i++) {
            a->hw_info.acquired_hw_id[i]=i+1;
            for(unsigned j=0;j<CAM_MAX_HW_SPLIT;j++)a->hw_info.acquired_hw_path[i][j]=i*3+j;
        }
        a->hw_info.valid_acquired_hw=1;
    }
    if(failure<0){errno=-failure;return -1;}return failure;
}
int main(void)
{
    _Static_assert(offsetof(struct cam_isp_acquire_hw_info,data)+sizeof(struct cam_isp_in_port_info_v2)==QUEST_CAMERA_ISP_ACQUIRE_SIZE,"Quest v2 resource extent");
    _Static_assert(sizeof(struct cam_csiphy_info)==QUEST_CAMERA_PHY_CONFIG_SIZE,"Quest PHY config extent");
    struct quest_camera_isp_config c={.lanes=4,.lane_map=0x3210,.width=1280,.height=1024,.bits=8};
    for(c.phy=0;c.phy<=6;c.phy++)for(c.rdi=0;c.rdi<4;c.rdi++)for(c.virtual_channel=0;c.virtual_channel<4;c.virtual_channel++)
        for(c.bits=8;c.bits<=10;c.bits+=2)for(c.output=QUEST_CAMERA_RAW_MIPI;c.output<=QUEST_CAMERA_RAW_PLAIN16;c.output++) {
            if(c.bits==10 && c.output==QUEST_CAMERA_RAW_PLAIN8)continue;
            check(&c);
        }
    struct quest_camera_phy_config p={.settle_count=65535,.data_rate=UINT64_MAX,.mipi_flags=6};
    for(p.cphy=0;p.cphy<2;p.cphy++)for(p.combo=0;p.combo<2;p.combo++)for(p.lanes=1;p.lanes<=4;p.lanes++)
        for(p.lane_map=0;p.lane_map<=UINT16_MAX;p.lane_map++) {
            bool valid=p.lanes<=(p.combo?(p.cphy?1u:2u):(p.cphy?3u:4u));
            unsigned selected[4]={0},seen=0;
            for(unsigned i=0;i<4;i++) {
                selected[i]=(p.lane_map>>(4*i))&15;
                if(i>=p.lanes){if(selected[i])valid=false;continue;}
                if(selected[i]>(p.cphy || p.combo?2u:3u) || (seen&(1u<<selected[i])))valid=false;
                seen|=1u<<selected[i];
            }
            uint8_t actual[QUEST_CAMERA_PHY_CONFIG_SIZE+2];memset(actual,0xa5,sizeof(actual));
            assert(quest_camera_phy_encode(&p,actual+1,sizeof(actual)-2)==valid);
            if(valid) {
                struct cam_csiphy_info expected={.lane_assign=(uint16_t)p.lane_map,.mipi_flags=6,.lane_cnt=(uint8_t)p.lanes,
                    .settle_time=65535ULL*200000000,.data_rate=UINT64_MAX};
                assert(!memcmp(actual+1,&expected,sizeof(expected)));
                c=(struct quest_camera_isp_config){.phy=6,.cphy=p.cphy,.combo=p.combo,.lanes=p.lanes,.lane_map=p.lane_map,
                    .bits=8,.width=1,.height=4096,.rdi=3,.output=QUEST_CAMERA_RAW_MIPI};check(&c);
            } else for(unsigned i=0;i<sizeof(actual);i++)assert(actual[i]==0xa5);
            assert(actual[0]==0xa5 && actual[sizeof(actual)-1]==0xa5);cases++;
        }
    const int errors[]={0,-EIO,-EINTR,-EFAULT,-ENODEV,1};
    for(unsigned e=0;e<6;e++) {
        failure=errors[e];int expected=failure>0?-EPROTO:failure;
        struct quest_camera_isp_hardware h;memset(&h,0xa5,sizeof(h));struct quest_camera_isp_hardware before=h;
        release_hw=false;assert(quest_camera_isp_hardware_acquire(31,101,201,resource,sizeof(resource),&h)==expected);
        if(expected)assert(!memcmp(&h,&before,sizeof(h)));
        else {assert(h.valid==1);for(unsigned i=0;i<5;i++){assert(h.id[i]==i+1);for(unsigned j=0;j<3;j++)assert(h.paths[i][j]==i*3+j);}}
        release_hw=true;assert(quest_camera_isp_hardware_release(31,101,201)==expected);
    }
    struct quest_camera_isp_hardware h={0};unsigned before=calls;
    assert(quest_camera_isp_hardware_acquire(31,101,201,resource,sizeof(resource)-1,&h)==-EINVAL);
    assert(quest_camera_isp_hardware_acquire(31,101,201,NULL,sizeof(resource),&h)==-EINVAL);
    assert(quest_camera_isp_hardware_release(31,0,201)==-EINVAL && before==calls);
    c=(struct quest_camera_isp_config){.lanes=4,.lane_map=0x3210,.bits=8,.width=1280,.height=1024};
    uint8_t output[QUEST_CAMERA_ISP_ACQUIRE_SIZE];memset(output,0xa5,sizeof(output));
    assert(!quest_camera_isp_encode(&c,output,sizeof(output)-1));c.phy=7;
    assert(!quest_camera_isp_encode(&c,output,sizeof(output)));c.phy=0;c.rdi=4;
    assert(!quest_camera_isp_encode(&c,output,sizeof(output)));c.rdi=0;c.width=4097;
    assert(!quest_camera_isp_encode(&c,output,sizeof(output)));c.width=1280;c.bits=12;
    assert(!quest_camera_isp_encode(&c,output,sizeof(output)));
    for(unsigned i=0;i<sizeof(output);i++)assert(output[i]==0xa5);
    p=(struct quest_camera_phy_config){.lanes=1,.data_rate=1,.settle_count=65536};
    assert(!quest_camera_phy_encode(&p,output,sizeof(output)));p.settle_count=0;p.mipi_flags=1;
    assert(!quest_camera_phy_encode(&p,output,sizeof(output)));p.mipi_flags=0;p.data_rate=0;
    assert(!quest_camera_phy_encode(&p,output,sizeof(output)));
    printf("QUEST_CAMERA_ISP_PASS cases=%u ioctls=%u hardware=0\n",cases,calls);
}
