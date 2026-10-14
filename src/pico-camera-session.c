#include "pico-camera-session.h"
#include <errno.h>
#include <stdlib.h>

struct pico_camera_session {
    struct pico_camera_session_config config;
    struct pico_camera_session_view view;
    bool hardware_attempted[2], uncertain, attempted, ready, quiesced, releasing;
    int release_error;
};
static bool valid_config(const struct pico_camera_session_config *c, uint8_t resource[2][PICO_CAMERA_IFE_ACQUIRE_SIZE])
{
    if (!c || c->crm_fd < 0 || c->ife_fd < 0 || c->crm_fd == c->ife_fd || c->master > 1) return false;
    int fds[6] = {c->crm_fd, c->ife_fd, c->cameras[0].sensor_fd, c->cameras[0].phy_fd,
                 c->cameras[1].sensor_fd, c->cameras[1].phy_fd};
    for (unsigned i = 0; i < 6; ++i) {
        if (fds[i] < 0) return false;
        for (unsigned j = 0; j < i; ++j) if (fds[i] == fds[j]) return false;
    }
    if (c->cameras[0].sensor_slot == c->cameras[1].sensor_slot ||
        c->cameras[0].input.phy == c->cameras[1].input.phy) return false;
    for (unsigned i = 0; i < 2; ++i)
        if (c->cameras[i].sensor_slot > 5 || !c->cameras[i].phy_version ||
            !pico_camera_ife_acquire(&c->cameras[i].input, resource[i], PICO_CAMERA_IFE_ACQUIRE_SIZE)) return false;
    return true;
}
static int query(const struct pico_camera_session_config *c, struct pico_camera_ife_capabilities *ife)
{
    for (unsigned i = 0; i < 2; ++i) {
        struct pico_camera_sensor_capabilities sensor;
        struct pico_camera_phy_capabilities phy;
        int result = pico_camera_sensor_query(c->cameras[i].sensor_fd, &sensor);
        if (result) return result;
        if (sensor.secure || sensor.slot != c->cameras[i].sensor_slot || sensor.phy_slot != c->cameras[i].input.phy)
            return -ENODEV;
        result = pico_camera_phy_query(c->cameras[i].phy_fd, &phy);
        if (result) return result;
        if (phy.slot != c->cameras[i].input.phy || phy.version != c->cameras[i].phy_version) return -ENODEV;
    }
    int result = pico_camera_ife_query(c->ife_fd, ife);
    return result ? result : ife->device_mmu < 0 || ife->cdm_mmu < 0 || !ife->device_count ? -ENODEV : 0;
}
static bool single_rdi0(const struct pico_camera_ife_hardware *h)
{
    if (h->valid != 1 || !h->id[0] || h->id[0] > 0x40 || (h->id[0] & (h->id[0] - 1)) ||
        h->paths[0][0] != 8 || h->paths[0][1] || h->paths[0][2]) return false;
    for (unsigned i = 1; i < 5; ++i)
        if (h->id[i] || h->paths[i][0] || h->paths[i][1] || h->paths[i][2]) return false;
    return true;
}
int pico_camera_session_acquire(const struct pico_camera_session_config *c,
                                struct pico_camera_session **session)
{
    uint8_t resources[2][PICO_CAMERA_IFE_ACQUIRE_SIZE];
    if (!session || *session || !valid_config(c, resources)) return -EINVAL;
    struct pico_camera_ife_capabilities capabilities;
    int result = query(c, &capabilities);
    if (result) return result;
    struct pico_camera_session *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    *session = s; s->config = *c; s->view.capabilities = capabilities;
    s->attempted = true;
    result = pico_camera_session_create(c->crm_fd, &s->view.session);
    if (result) { s->uncertain = s->view.session <= 0; return result; }
    for (unsigned i = 0; i < 2; ++i) {
        result = pico_camera_sensor_acquire(c->cameras[i].sensor_fd, s->view.session, &s->view.cameras[i].sensor);
        if (result) { s->uncertain = s->view.cameras[i].sensor <= 0; return result; }
        result = pico_camera_phy_acquire(c->cameras[i].phy_fd, s->view.session, false, &s->view.cameras[i].phy);
        if (result) { s->uncertain = s->view.cameras[i].phy <= 0; return result; }
        result = pico_camera_ife_device_acquire(c->ife_fd, s->view.session, &s->view.cameras[i].ife);
        if (result) { s->uncertain = s->view.cameras[i].ife <= 0; return result; }
        s->hardware_attempted[i] = true;
        result = pico_camera_ife_hardware_acquire(c->ife_fd, s->view.session, s->view.cameras[i].ife,
                                                 resources[i], sizeof(resources[i]), &s->view.cameras[i].hardware);
        if (result) return result;
        if (!single_rdi0(&s->view.cameras[i].hardware)) return -EPROTO;
    }
    if (s->view.cameras[0].hardware.id[0] == s->view.cameras[1].hardware.id[0]) return -EPROTO;
    for (unsigned i = 0; i < 2; ++i) {
        int32_t devices[] = {s->view.cameras[i].sensor, s->view.cameras[i].ife};
        result = pico_camera_link_create(c->crm_fd, s->view.session, devices, 2, &s->view.cameras[i].link);
        if (result) { s->uncertain = s->view.cameras[i].link <= 0; return result; }
    }
    int32_t links[] = {s->view.cameras[0].link, s->view.cameras[1].link};
    result = pico_camera_links_synchronize(c->crm_fd, s->view.session, links, links[c->master]);
    if (result) return result;
    s->ready = true;
    return 0;
}
bool pico_camera_session_view(const struct pico_camera_session *s, struct pico_camera_session_view *view)
{
    if (!s || !s->ready || s->quiesced || s->releasing || !view) return false;
    *view = s->view;
    return true;
}
int pico_camera_session_quiesced(struct pico_camera_session *s)
{
    if (!s || s->quiesced) return -EINVAL;
    s->quiesced = true; s->ready = false;
    return 0;
}
static void error(struct pico_camera_session *s, int result)
{ if (result && !s->release_error) s->release_error = result; }
int pico_camera_session_release(struct pico_camera_session **session)
{
    if (!session) return -EINVAL;
    struct pico_camera_session *s = *session;
    if (!s) return 0;
    if (s->attempted && !s->quiesced) return -EBUSY;
    if (s->releasing) return s->release_error;
    s->releasing = true; s->ready = false;
    if (s->uncertain) { s->release_error = -EIO; return s->release_error; }
    for (unsigned i = 0; i < 2; ++i)
        if (s->view.cameras[i].link > 0)
            error(s, pico_camera_link_destroy(s->config.crm_fd, s->view.session, &s->view.cameras[i].link));
    if (s->release_error) return s->release_error;
    for (unsigned i = 0; i < 2; ++i) {
        if (s->hardware_attempted[i]) {
            int result = pico_camera_ife_hardware_release(s->config.ife_fd, s->view.session, s->view.cameras[i].ife);
            error(s, result);
            if (!result) s->hardware_attempted[i] = false;
        }
        if (s->view.cameras[i].ife > 0 && !s->hardware_attempted[i])
            error(s, pico_camera_device_release(s->config.ife_fd, s->view.session, &s->view.cameras[i].ife));
        if (s->view.cameras[i].phy > 0)
            error(s, pico_camera_device_release(s->config.cameras[i].phy_fd, s->view.session, &s->view.cameras[i].phy));
        if (s->view.cameras[i].sensor > 0)
            error(s, pico_camera_device_release(s->config.cameras[i].sensor_fd, s->view.session, &s->view.cameras[i].sensor));
    }
    if (s->release_error) return s->release_error;
    if (s->view.session > 0) error(s, pico_camera_session_destroy(s->config.crm_fd, &s->view.session));
    if (s->release_error) return s->release_error;
    free(s); *session = NULL;
    return 0;
}
void pico_camera_session_closed(struct pico_camera_session **session)
{
    if (!session) return;
    free(*session); *session = NULL;
}
