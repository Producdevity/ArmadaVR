#include "quest-camera-group.h"
#include <errno.h>
#include <stdlib.h>

struct quest_camera_group {
    struct quest_camera_group_config config;
    struct quest_camera_group_view view;
    bool hardware_attempted[4], uncertain, ready, quiesced, releasing;
    int release_error;
};
static bool valid_config(const struct quest_camera_group_config *c,
                         uint8_t resources[4][QUEST_CAMERA_ISP_ACQUIRE_SIZE])
{
    if (!c || !c->count || c->count > 4 || c->master >= c->count ||
        (!c->synchronized && c->master) || (c->synchronized && c->count < 2) ||
        (c->mode != QUEST_CAMERA_LINK_STREAMING && c->mode != QUEST_CAMERA_LINK_TRIGGERED)) return false;
    int fds[10] = {c->crm_fd, c->isp_fd};
    for (size_t i = 0; i < c->count; ++i) {
        fds[2 + i * 2] = c->cameras[i].sensor_fd; fds[3 + i * 2] = c->cameras[i].phy_fd;
        if (c->cameras[i].sensor_slot > 5 || !c->cameras[i].phy_version || c->cameras[i].input.combo ||
            !quest_camera_isp_encode(&c->cameras[i].input, resources[i], QUEST_CAMERA_ISP_ACQUIRE_SIZE)) return false;
        for (size_t j = 0; j < i; ++j)
            if (c->cameras[i].sensor_slot == c->cameras[j].sensor_slot ||
                c->cameras[i].input.phy == c->cameras[j].input.phy) return false;
    }
    for (size_t i = 0; i < 2 + c->count * 2; ++i) {
        if (fds[i] < 0) return false;
        for (size_t j = 0; j < i; ++j) if (fds[i] == fds[j]) return false;
    }
    return true;
}
static int query(const struct quest_camera_group_config *c, struct quest_camera_isp_capabilities *isp)
{
    for (size_t i = 0; i < c->count; ++i) {
        struct quest_camera_sensor_capabilities sensor;
        struct quest_camera_phy_capabilities phy;
        int result = quest_camera_sensor_query(c->cameras[i].sensor_fd, &sensor);
        if (result) return result;
        if (sensor.secure || sensor.slot != c->cameras[i].sensor_slot || sensor.phy_slot != c->cameras[i].input.phy)
            return -ENODEV;
        result = quest_camera_phy_query(c->cameras[i].phy_fd, &phy);
        if (result) return result;
        if (phy.slot != c->cameras[i].input.phy || phy.version != c->cameras[i].phy_version) return -ENODEV;
    }
    int result = quest_camera_isp_query(c->isp_fd, isp);
    return result ? result : isp->device_mmu < 0 || isp->cdm_mmu < 0 || !isp->device_count ? -ENODEV : 0;
}
static bool single_rdi(const struct quest_camera_isp_hardware *h, unsigned rdi)
{
    if (h->valid != 1 || !h->id[0] || (h->id[0] & ~0xf01ffu) || (h->id[0] & (h->id[0] - 1)) ||
        h->paths[0][0] != (8u << rdi) || h->paths[0][1] || h->paths[0][2]) return false;
    for (unsigned i = 1; i < 5; ++i)
        if (h->id[i] || h->paths[i][0] || h->paths[i][1] || h->paths[i][2]) return false;
    return true;
}
static int acquired(struct quest_camera_group *s, int result, int32_t handle)
{
    if (handle <= 0) { s->uncertain = true; return result ? result : -EPROTO; }
    unsigned occurrences = 0;
    for (size_t i = 0; i < s->config.count; ++i) {
        occurrences += s->view.cameras[i].sensor == handle;
        occurrences += s->view.cameras[i].phy == handle;
        occurrences += s->view.cameras[i].isp == handle;
    }
    if (occurrences > 1) { s->uncertain = true; return -EPROTO; }
    return result;
}
int quest_camera_group_acquire(const struct quest_camera_group_config *c, struct quest_camera_group **group)
{
    uint8_t resources[4][QUEST_CAMERA_ISP_ACQUIRE_SIZE];
    if (!group || *group || !valid_config(c, resources)) return -EINVAL;
    struct quest_camera_isp_capabilities capabilities;
    int result = query(c, &capabilities);
    if (result) return result;
    struct quest_camera_group *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    *group = s; s->config = *c; s->view.count = c->count; s->view.capabilities = capabilities;
    result = quest_camera_session_create(c->crm_fd, &s->view.session);
    if (result) { s->uncertain = s->view.session <= 0; return result; }
    for (size_t i = 0; i < c->count; ++i) {
        result = quest_camera_sensor_acquire(c->cameras[i].sensor_fd, s->view.session, &s->view.cameras[i].sensor);
        result = acquired(s, result, s->view.cameras[i].sensor);
        if (result) return result;
        result = quest_camera_phy_acquire(c->cameras[i].phy_fd, s->view.session, false,
                                          c->cameras[i].input.cphy, &s->view.cameras[i].phy);
        result = acquired(s, result, s->view.cameras[i].phy);
        if (result) return result;
        result = quest_camera_isp_device_acquire(c->isp_fd, s->view.session, &s->view.cameras[i].isp);
        result = acquired(s, result, s->view.cameras[i].isp);
        if (result) return result;
        s->hardware_attempted[i] = true;
        result = quest_camera_isp_hardware_acquire(c->isp_fd, s->view.session, s->view.cameras[i].isp,
                                                 resources[i], sizeof(resources[i]), &s->view.cameras[i].hardware);
        if (result) return result;
        const struct quest_camera_isp_hardware *h = &s->view.cameras[i].hardware;
        if (!single_rdi(h, c->cameras[i].input.rdi)) return -EPROTO;
        for (size_t j = 0; j < i; ++j)
            if (s->view.cameras[j].hardware.id[0] == h->id[0] &&
                (s->view.cameras[j].hardware.paths[0][0] & h->paths[0][0])) return -EPROTO;
    }
    for (size_t i = 0; i < c->count; ++i) {
        int32_t devices[] = {s->view.cameras[i].sensor, s->view.cameras[i].isp};
        result = quest_camera_link_create_v3(c->crm_fd, s->view.session, devices, 2,
                                              c->mode, c->independent, &s->view.cameras[i].link);
        for (size_t j = 0; j < i; ++j)
            if (s->view.cameras[i].link > 0 && s->view.cameras[i].link == s->view.cameras[j].link) {
                s->uncertain = true; return -EPROTO;
            }
        if (result) { s->uncertain = s->view.cameras[i].link <= 0; return result; }
    }
    if (c->synchronized) {
        struct quest_camera_sync_link links[4] = {0};
        for (size_t i = 0; i < c->count; ++i)
            links[i] = (struct quest_camera_sync_link){s->view.cameras[i].link, (int32_t)c->cameras[i].sensor_slot, false};
        result = quest_camera_links_synchronize_v2(c->crm_fd, s->view.session, links, c->count,
                                                    true, s->view.cameras[c->master].link);
        if (result) return result;
    }
    s->ready = true;
    return 0;
}
bool quest_camera_group_view(const struct quest_camera_group *s, struct quest_camera_group_view *view)
{
    if (!s || !s->ready || s->quiesced || s->releasing || !view) return false;
    *view = s->view;
    return true;
}
int quest_camera_group_quiesced(struct quest_camera_group *s)
{
    if (!s || s->quiesced) return -EINVAL;
    s->quiesced = true; s->ready = false;
    return 0;
}
static void error(struct quest_camera_group *s, int result)
{ if (result && !s->release_error) s->release_error = result; }
int quest_camera_group_release(struct quest_camera_group **group)
{
    if (!group) return -EINVAL;
    struct quest_camera_group *s = *group;
    if (!s) return 0;
    if (!s->quiesced) return -EBUSY;
    if (s->releasing) return s->release_error;
    s->releasing = true; s->ready = false;
    if (s->uncertain) { s->release_error = -EIO; return s->release_error; }
    for (size_t i = 0; i < s->config.count; ++i)
        if (s->view.cameras[i].link > 0)
            error(s, quest_camera_link_destroy(s->config.crm_fd, s->view.session, &s->view.cameras[i].link));
    if (s->release_error) return s->release_error;
    for (size_t i = 0; i < s->config.count; ++i) {
        if (s->hardware_attempted[i]) {
            int result = quest_camera_isp_hardware_release(s->config.isp_fd, s->view.session, s->view.cameras[i].isp);
            error(s, result);
            if (!result) s->hardware_attempted[i] = false;
        }
        if (s->view.cameras[i].isp > 0 && !s->hardware_attempted[i])
            error(s, quest_camera_device_release(s->config.isp_fd, s->view.session, &s->view.cameras[i].isp));
        if (s->view.cameras[i].phy > 0)
            error(s, quest_camera_device_release(s->config.cameras[i].phy_fd, s->view.session, &s->view.cameras[i].phy));
        if (s->view.cameras[i].sensor > 0)
            error(s, quest_camera_device_release(s->config.cameras[i].sensor_fd, s->view.session, &s->view.cameras[i].sensor));
    }
    if (s->release_error) return s->release_error;
    if (s->view.session > 0) error(s, quest_camera_session_destroy(s->config.crm_fd, &s->view.session));
    if (s->release_error) return s->release_error;
    free(s); *group = NULL;
    return 0;
}
void quest_camera_group_closed(struct quest_camera_group **group)
{
    if (group) { free(*group); *group = NULL; }
}
