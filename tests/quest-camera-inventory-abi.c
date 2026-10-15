#include <sys/types.h>
#include <media/cam_req_mgr.h>
#include <media/cam_sensor.h>
#include <media/cam_sync.h>
#include <stddef.h>
#include <stdio.h>

int main(void)
{
    unsigned roles[] = {CAM_VNODE_DEVICE_TYPE, CAM_SENSOR_DEVICE_TYPE, CAM_IFE_DEVICE_TYPE,
        CAM_ICP_DEVICE_TYPE, CAM_LRME_DEVICE_TYPE, CAM_JPEG_DEVICE_TYPE, CAM_FD_DEVICE_TYPE,
        CAM_CPAS_DEVICE_TYPE, CAM_CSIPHY_DEVICE_TYPE, CAM_ACTUATOR_DEVICE_TYPE,
        CAM_CCI_DEVICE_TYPE, CAM_FLASH_DEVICE_TYPE, CAM_EEPROM_DEVICE_TYPE, CAM_OIS_DEVICE_TYPE,
        CAM_CUSTOM_DEVICE_TYPE, CAM_OPE_DEVICE_TYPE, CAM_TFE_DEVICE_TYPE, CAM_CRE_DEVICE_TYPE,
        CAM_TPG_DEVICE_TYPE, CAM_SENSORLITE_DEVICE_TYPE, CAM_CSIPHY_REMOTE_DEVICE_TYPE};
    printf("{\"roles\":[");
    for (unsigned i=0;i<sizeof(roles)/sizeof(roles[0]);++i) printf("%s%u",i?",":"",roles[i]);
    printf("],\"models\":[\"%s\",\"%s\"],\"sync_role\":%u,\"control\":%lu,"
           "\"create_session\":%u,\"link\":%u,\"schedule\":%u,\"sizes\":[%zu,%zu,%zu,%zu,%zu]}\n",
           CAM_REQ_MGR_VNODE_NAME,CAM_SYNC_DEVICE_NAME,CAM_SYNC_DEVICE_TYPE,(unsigned long)VIDIOC_CAM_CONTROL,
           CAM_REQ_MGR_CREATE_SESSION,CAM_REQ_MGR_LINK,CAM_REQ_MGR_SCHED_REQ,
           sizeof(struct cam_control),sizeof(struct cam_req_mgr_session_info),sizeof(struct cam_req_mgr_link_info),
           sizeof(struct cam_sensor_query_cap),sizeof(struct cam_csiphy_query_cap));
}
