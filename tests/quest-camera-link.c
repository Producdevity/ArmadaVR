#include "quest-camera-control.h"
#include <sys/types.h>
#include <media/cam_defs.h>
#include <media/cam_req_mgr.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

static unsigned calls, cases, opcode, count, mode;
static int error, returned, master;
static bool independent, enabled;
int ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 41 && request == VIDIOC_CAM_CONTROL);
    va_list ap; va_start(ap, request); struct cam_control *c = va_arg(ap, void *); va_end(ap);
    assert(c->op_code == opcode && c->handle_type == CAM_HANDLE_USER_POINTER && !c->reserved && c->handle);
    void *p = (void *)(uintptr_t)c->handle; calls++;
    if (opcode == CAM_REQ_MGR_LINK_V3) {
        struct cam_req_mgr_link_info_v3 *v = p;
        assert(c->size == sizeof(*v) && v->session_hdl == 17 && v->num_devices == count && !v->link_hdl);
        unsigned expected = mode == QUEST_CAMERA_LINK_STREAMING ? CAM_REQ_MGR_LINK_STREAMING_TYPE : CAM_REQ_MGR_LINK_TRIGGER_TYPE;
        if (independent) expected |= CAM_REQ_MGR_LINK_INDEPENDENT_CRM_TYPE;
        assert(v->trigger_type == expected);
        for (unsigned i = 0; i < CAM_REQ_MGR_MAX_HANDLES_V2; i++) assert(v->dev_hdls[i] == (i < count ? 100 + (int)i : 0));
        v->link_hdl = returned;
    } else {
        assert(opcode == CAM_REQ_MGR_SYNC_MODE_V2);
        struct cam_req_mgr_sync_mode_v2 *v = p;
        assert(c->size == sizeof(*v) + (count - 1) * sizeof(struct cam_req_mgr_sync_link_desc));
        assert(v->version == 2 && v->session_hdl == 17 && v->num_links == (int)count && !v->reserved && v->master_link_hdl == master);
        assert(v->sync_mode == (enabled ? CAM_REQ_MGR_SYNC_MODE_SYNC : CAM_REQ_MGR_SYNC_MODE_NO_SYNC));
        struct cam_req_mgr_sync_link_desc *links = (void *)((unsigned char *)p + offsetof(struct cam_req_mgr_sync_mode_v2, links));
        for (unsigned i = 0; i < count; i++) assert(links[i].link_hdl == 100 + (int)i && links[i].sensor_id == (int)i && links[i].remote_sensor == i % 2 && !links[i].reserved);
    }
    if (error > 0) { errno = error; return -1; }
    return error ? 1 : 0;
}
int main(void)
{
    const int errors[] = {0, EIO, EFAULT, EINTR, ENODEV, -1}, handles[] = {0, -1, 19, INT32_MAX};
    int32_t devices[8]; struct quest_camera_sync_link links[16];
    for (unsigned i = 0; i < 8; i++) devices[i] = 100 + i;
    for (unsigned i = 0; i < 16; i++) links[i] = (struct quest_camera_sync_link){100 + (int)i, (int)i, i % 2};
    for (unsigned e = 0; e < sizeof(errors) / sizeof(errors[0]); e++) {
        error = errors[e]; int expected_error = error > 0 ? -error : error ? -EPROTO : 0;
        opcode = CAM_REQ_MGR_LINK_V3;
        for (unsigned h = 0; h < sizeof(handles) / sizeof(handles[0]); h++) {
            returned = handles[h];
            int expected = expected_error ? expected_error : returned > 0 ? 0 : -EPROTO;
            for (count = 1; count <= 8; count++) for (mode = 1; mode <= 2; mode++) for (unsigned flags = 0; flags < 2; flags++) {
                independent = flags; int32_t link = 0;
                assert(quest_camera_link_create_v3(41, 17, devices, count, mode, independent, &link) == expected);
                assert(link == (returned > 0 ? returned : 0)); cases++;
            }
        }
        opcode = CAM_REQ_MGR_SYNC_MODE_V2;
        for (count = 1; count <= 16; count++) {
            for (unsigned index = 0; index <= count; index++) {
                enabled = index < count; master = enabled ? links[index].link : 0;
                assert(quest_camera_links_synchronize_v2(41, 17, links, count, enabled, master) == expected_error); cases++;
            }
        }
    }
    unsigned before = calls; int32_t link = 0, positive = 1;
#define BAD(v) do { assert((v) == -EINVAL); cases++; } while (0)
    BAD(quest_camera_link_create_v3(-1,17,devices,1,1,false,&link));
    BAD(quest_camera_link_create_v3(41,0,devices,1,1,false,&link));
    BAD(quest_camera_link_create_v3(41,17,NULL,1,1,false,&link));
    BAD(quest_camera_link_create_v3(41,17,devices,0,1,false,&link));
    BAD(quest_camera_link_create_v3(41,17,devices,9,1,false,&link));
    BAD(quest_camera_link_create_v3(41,17,devices,SIZE_MAX,1,false,&link));
    BAD(quest_camera_link_create_v3(41,17,devices,1,1,false,NULL));
    BAD(quest_camera_link_create_v3(41,17,devices,1,1,false,&positive));
    for (int m = -10; m <= 10; m++) if (m != 1 && m != 2) BAD(quest_camera_link_create_v3(41,17,devices,1,m,false,&link));
    for (unsigned i = 0; i < 8; i++) {
        int32_t saved = devices[i]; devices[i] = 0; BAD(quest_camera_link_create_v3(41,17,devices,8,1,false,&link));
        devices[i] = -1; BAD(quest_camera_link_create_v3(41,17,devices,8,1,false,&link)); devices[i] = saved;
        for (unsigned j = 0; j < i; j++) { devices[i] = devices[j]; BAD(quest_camera_link_create_v3(41,17,devices,8,1,false,&link)); devices[i] = saved; }
    }
    BAD(quest_camera_links_synchronize_v2(-1,17,links,16,true,100));
    BAD(quest_camera_links_synchronize_v2(41,0,links,16,true,100));
    BAD(quest_camera_links_synchronize_v2(41,17,NULL,16,true,100));
    BAD(quest_camera_links_synchronize_v2(41,17,links,0,true,100));
    BAD(quest_camera_links_synchronize_v2(41,17,links,17,true,100));
    BAD(quest_camera_links_synchronize_v2(41,17,links,SIZE_MAX,true,100));
    BAD(quest_camera_links_synchronize_v2(41,17,links,16,false,100));
    BAD(quest_camera_links_synchronize_v2(41,17,links,16,true,0));
    BAD(quest_camera_links_synchronize_v2(41,17,links,16,true,116));
    for (unsigned i = 0; i < 16; i++) {
        struct quest_camera_sync_link saved = links[i]; links[i].link = 0; BAD(quest_camera_links_synchronize_v2(41,17,links,16,true,100));
        links[i] = saved; links[i].sensor_id = -1; BAD(quest_camera_links_synchronize_v2(41,17,links,16,true,100)); links[i] = saved;
        for (unsigned j = 0; j < i; j++) { links[i].link = links[j].link; BAD(quest_camera_links_synchronize_v2(41,17,links,16,true,100)); links[i] = saved; }
    }
    assert(calls == before && calls == 1680 && cases == 1912);
    printf("QUEST_CAMERA_LINK_PASS ioctls=%u cases=%u hardware=0 preserved_failure_handles=1\n",calls,cases);
}
