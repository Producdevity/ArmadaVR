#ifndef REFRAMED_QUEST_CAMERA_CONTROL_H
#define REFRAMED_QUEST_CAMERA_CONTROL_H

#include <stdint.h>

struct quest_camera_sensor_capabilities {
    uint32_t slot, secure, pitch, roll, yaw;
    uint32_t actuator_slot, eeprom_slot, ois_slot, flash_slot, phy_slot;
};
struct quest_camera_phy_capabilities { uint32_t slot, version, clock_lane; };
struct quest_camera_isp_capabilities {
    int32_t device_mmu, cdm_mmu;
    uint32_t device_count;
    struct { uint32_t type, count, major, minor, increment; } devices[8];
};

/* Meta dbc2bc83 camera UAPI. The owner supplies already identified descriptors;
 * opening or closing CRM has global side effects. These queries neither acquire
 * nor start a sensor, retry ioctls, or replace output after failure. */
int quest_camera_sensor_query(int fd, struct quest_camera_sensor_capabilities *caps);
int quest_camera_phy_query(int fd, struct quest_camera_phy_capabilities *caps);
int quest_camera_isp_query(int fd, struct quest_camera_isp_capabilities *caps);

#endif
