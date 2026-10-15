#include "quest-camera-control.h"
#include <sys/types.h>
#include <media/cam_defs.h>
#include <media/cam_req_mgr.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

static unsigned calls, expected_opcode, expected_count, cases;
static int failure, returned_handle;
static bool active, sync_mode, bubble;
static int32_t master;
static uint64_t request_id;
int ioctl(int fd, unsigned long request, ...)
{
    assert(fd==41 && request==VIDIOC_CAM_CONTROL);
    va_list ap;va_start(ap,request);struct cam_control *c=va_arg(ap,void *);va_end(ap);
    assert(c->op_code==expected_opcode && c->handle_type==CAM_HANDLE_USER_POINTER && !c->reserved && c->handle);
    void *p=(void *)(uintptr_t)c->handle;calls++;
    switch(c->op_code) {
    case CAM_REQ_MGR_CREATE_SESSION:
    case CAM_REQ_MGR_DESTROY_SESSION: {
        struct cam_req_mgr_session_info *v=p;assert(c->size==sizeof(*v) && !v->reserved);
        assert(v->session_hdl==(c->op_code==CAM_REQ_MGR_CREATE_SESSION?0:17));
        if(c->op_code==CAM_REQ_MGR_CREATE_SESSION)v->session_hdl=returned_handle;
        break;
    }
    case CAM_REQ_MGR_LINK: {
        struct cam_req_mgr_link_info *v=p;assert(c->size==sizeof(*v) && v->session_hdl==17 && v->num_devices==expected_count && !v->link_hdl);
        for(unsigned i=0;i<CAM_REQ_MGR_MAX_HANDLES;i++)assert(v->dev_hdls[i]==(i<expected_count?100+(int)i:0));
        v->link_hdl=returned_handle;break;
    }
    case CAM_REQ_MGR_UNLINK: {
        struct cam_req_mgr_unlink_info *v=p;assert(c->size==sizeof(*v) && v->session_hdl==17 && v->link_hdl==23);break;
    }
    case CAM_REQ_MGR_LINK_CONTROL: {
        struct cam_req_mgr_link_control *v=p;assert(c->size==sizeof(*v) && v->session_hdl==17 && v->num_links==1 && !v->reserved);
        assert(v->ops==(active?CAM_REQ_MGR_LINK_ACTIVATE:CAM_REQ_MGR_LINK_DEACTIVATE));
        assert(!v->init_timeout[0] && !v->init_timeout[1] && v->link_hdls[0]==23 && !v->link_hdls[1]);break;
    }
    case CAM_REQ_MGR_SYNC_MODE: {
        struct cam_req_mgr_sync_mode *v=p;assert(c->size==sizeof(*v) && v->session_hdl==17 && v->num_links==2 && !v->reserved);
        assert(v->sync_mode==CAM_REQ_MGR_SYNC_MODE_SYNC && v->link_hdls[0]==23 && v->link_hdls[1]==24 && v->master_link_hdl==master);break;
    }
    case CAM_REQ_MGR_SCHED_REQ: {
        struct cam_req_mgr_sched_request *v=p;assert(c->size==sizeof(*v) && v->session_hdl==17 && v->link_hdl==23);
        assert(!v->additional_timeout && !v->reserved && v->req_id==(int64_t)request_id);
        assert(v->sync_mode==(sync_mode?CAM_REQ_MGR_SYNC_MODE_SYNC:CAM_REQ_MGR_SYNC_MODE_NO_SYNC) && v->bubble_enable==(int)bubble);break;
    }
    case CAM_REQ_MGR_FLUSH_REQ: {
        struct cam_req_mgr_flush_info *v=p;assert(c->size==sizeof(*v) && v->session_hdl==17 && v->link_hdl==23);
        assert(v->flush_type==CAM_REQ_MGR_FLUSH_TYPE_ALL && !v->reserved && !v->req_id);break;
    }
    default:assert(0);
    }
    if(failure>0){errno=failure;return -1;}
    return failure?1:0;
}
int main(void)
{
    _Static_assert(MAX_LINKS_PER_SESSION==2,"Quest legacy link count");
    int errors[]={0,EIO,EFAULT,EINTR,ENODEV,-1}, handles[]={0,-1,19,INT32_MAX};
    int32_t devices[8];for(unsigned i=0;i<8;i++)devices[i]=100+i;
    for(unsigned e=0;e<sizeof(errors)/sizeof(errors[0]);e++) {
        failure=errors[e];int want=failure>0?-failure:failure?-EPROTO:0;
        for(unsigned h=0;h<sizeof(handles)/sizeof(handles[0]);h++) {
            returned_handle=handles[h];int expected=want?want:returned_handle>0?0:-EPROTO;
            int32_t session=0;expected_opcode=CAM_REQ_MGR_CREATE_SESSION;
            assert(quest_camera_session_create(41,&session)==expected && session==(returned_handle>0?returned_handle:0));cases++;
            for(expected_count=2;expected_count<=8;expected_count++) {
                int32_t link=0;expected_opcode=CAM_REQ_MGR_LINK;
                assert(quest_camera_link_create(41,17,devices,expected_count,&link)==expected && link==(returned_handle>0?returned_handle:0));cases++;
            }
        }
        int32_t session=17,link=23;expected_opcode=CAM_REQ_MGR_DESTROY_SESSION;
        assert(quest_camera_session_destroy(41,&session)==want && session==(want?17:0));cases++;
        expected_opcode=CAM_REQ_MGR_UNLINK;assert(quest_camera_link_destroy(41,17,&link)==want && link==(want?23:0));cases++;
        expected_opcode=CAM_REQ_MGR_LINK_CONTROL;
        active=false;assert(quest_camera_link_activate(41,17,23,active)==want);cases++;
        active=true;assert(quest_camera_link_activate(41,17,23,active)==want);cases++;
        expected_opcode=CAM_REQ_MGR_SYNC_MODE;int32_t links[]={23,24};
        for(master=23;master<=24;master++){assert(quest_camera_links_synchronize(41,17,links,master)==want);cases++;}
        expected_opcode=CAM_REQ_MGR_SCHED_REQ;
        for(unsigned flags=0;flags<4;flags++)for(unsigned large=0;large<2;large++) {
            sync_mode=flags&1;bubble=flags&2;request_id=large?INT64_MAX:1;
            assert(quest_camera_request_schedule(41,17,23,request_id,sync_mode,bubble)==want);cases++;
        }
        expected_opcode=CAM_REQ_MGR_FLUSH_REQ;assert(quest_camera_requests_flush(41,17,23)==want);cases++;
    }
    unsigned before=calls;int32_t zero=0,positive=17,links[]={23,24};
#define BAD(v) do{assert((v)==-EINVAL);cases++;}while(0)
    BAD(quest_camera_session_create(-1,&zero));BAD(quest_camera_session_create(41,NULL));BAD(quest_camera_session_create(41,&positive));
    BAD(quest_camera_session_destroy(41,NULL));BAD(quest_camera_session_destroy(41,&zero));
    BAD(quest_camera_link_create(41,0,devices,2,&zero));BAD(quest_camera_link_create(41,17,NULL,2,&zero));
    BAD(quest_camera_link_create(41,17,devices,1,&zero));BAD(quest_camera_link_create(41,17,devices,9,&zero));
    BAD(quest_camera_link_create(41,17,devices,2,NULL));BAD(quest_camera_link_create(41,17,devices,2,&positive));
    devices[1]=devices[0];BAD(quest_camera_link_create(41,17,devices,2,&zero));devices[1]=0;BAD(quest_camera_link_create(41,17,devices,2,&zero));
    BAD(quest_camera_link_destroy(41,0,&positive));BAD(quest_camera_link_destroy(41,17,&zero));BAD(quest_camera_link_destroy(41,17,NULL));
    BAD(quest_camera_link_activate(41,0,23,true));BAD(quest_camera_link_activate(41,17,0,true));
    BAD(quest_camera_links_synchronize(41,17,NULL,23));BAD(quest_camera_links_synchronize(41,0,links,23));
    BAD(quest_camera_links_synchronize(41,17,links,25));links[1]=23;BAD(quest_camera_links_synchronize(41,17,links,23));
    links[1]=0;BAD(quest_camera_links_synchronize(41,17,links,23));
    BAD(quest_camera_request_schedule(41,0,23,1,false,false));BAD(quest_camera_request_schedule(41,17,0,1,false,false));
    BAD(quest_camera_request_schedule(41,17,23,0,false,false));BAD(quest_camera_request_schedule(41,17,23,UINT64_MAX,false,false));
    BAD(quest_camera_requests_flush(41,0,23));BAD(quest_camera_requests_flush(41,17,0));
    assert(calls==before && calls==282 && cases==311);
    printf("QUEST_CAMERA_REQUEST_PASS ioctls=%u cases=%u hardware=0 preserved_failure_handles=1\n",calls,cases);
}
