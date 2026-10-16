#ifndef REFRAMED_QUEST_CAMERA_GROUP_H
#define REFRAMED_QUEST_CAMERA_GROUP_H
#include "quest-camera-control.h"
#include "quest-camera-isp.h"

struct quest_camera_group;
struct quest_camera_group_config {
    int crm_fd, isp_fd;
    size_t count;
    unsigned master;
    bool synchronized, independent;
    enum quest_camera_link_mode mode;
    struct {
        int sensor_fd, phy_fd;
        uint32_t sensor_slot, phy_version;
        struct quest_camera_isp_config input;
    } cameras[4];
};
struct quest_camera_group_view {
    int32_t session;
    size_t count;
    struct quest_camera_isp_capabilities capabilities;
    struct {
        int32_t sensor, phy, isp, link;
        struct quest_camera_isp_hardware hardware;
    } cameras[4];
};

/* Acquire 1-4 explicitly selected, already probed local cameras in one CRM
 * session. Each has a distinct sensor and PHY, one IFE RDI path and a V3 link.
 * Remote/SFE, shared/combo PHY and split hardware inputs are excluded. Queries
 * must match every route/version before acquisition; no nodes are opened and no
 * sensor configuration, scheduling or stream start is implicit. Optional CRM
 * synchronization does not select the MCU hardware master. Descriptors are
 * borrowed exclusively for this owner and its children. Calls are serialized.
 * A nonnull owner must be retained even after failure. */
int quest_camera_group_acquire(const struct quest_camera_group_config *config,
                                struct quest_camera_group **group);
bool quest_camera_group_view(const struct quest_camera_group *group,
                              struct quest_camera_group_view *view);
/* Requires stopped scheduling/events, returned controls, independently confirmed
 * device/DMA quiescence and release of all children borrowing these handles. */
int quest_camera_group_quiesced(struct quest_camera_group *group);
/* One cleanup attempt; never retries uncertain operations or destroys an
 * enclosing session while children are unresolved. */
int quest_camera_group_release(struct quest_camera_group **group);
/* Local disposal only, after independent quiescence and closure of every
 * borrowed descriptor. Does not itself close fds or stop/release hardware. */
void quest_camera_group_closed(struct quest_camera_group **group);
#endif
