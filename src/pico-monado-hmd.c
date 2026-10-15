#include "pico-monado-hmd.h"
#include <stdlib.h>
#include <string.h>

struct pico_hmd_provider {
    struct pico_head_session *session;
    struct pico_slam_tracking *visual_tracking;
};

static bool get_pose(void *userdata, int64_t now, int64_t at, struct xrt_space_relation *relation)
{
    struct pico_hmd_provider *p = userdata;
    int64_t sample;
    return p->visual_tracking ? pico_slam_tracking_get(p->visual_tracking, now, at, relation, &sample) :
                               pico_head_session_get_at(p->session, now, at, relation);
}

static bool update_input(void *userdata, int64_t now, struct xrt_space_relation *relation, int64_t *sample)
{
    struct pico_hmd_provider *p = userdata;
    return p->visual_tracking ? pico_slam_tracking_get(p->visual_tracking, now, now, relation, sample) :
                               pico_head_session_get(p->session, now, relation, sample);
}

static bool release(void *userdata)
{
    struct pico_hmd_provider *p = userdata;
    pico_head_session_stop(p->session);
    if (!pico_head_session_destroy(&p->session)) return false;
    free(p);
    return true;
}

bool pico_monado_hmd_create(const struct pico_hmd_config *config, struct xrt_device **device)
{
    if (!config || !device || *device) return false;
    struct pico_hmd_provider *p = calloc(1, sizeof(*p));
    if (!p) return false;
    p->visual_tracking = config->visual_tracking;
    struct calibrated_hmd_config optics = {
        .serial = config->serial, .name = "Pico Neo3 HMD",
        .tracking_name = p->visual_tracking ? "Pico visual-inertial tracking" : "Pico head orientation",
        .screen_width = config->screen_width, .screen_height = config->screen_height,
        .frame_interval_ns = config->frame_interval_ns, .position_tracking = p->visual_tracking != NULL,
    };
    memcpy(optics.views, config->views, sizeof(optics.views));
    struct calibrated_hmd_provider provider = {p, get_pose, update_input, release};
    struct xrt_device *hmd = NULL;
    if (!calibrated_hmd_create(&optics, &provider, &hmd)) { free(p); return false; }
    if (!p->visual_tracking && !pico_head_session_create(&config->sensors, &config->tracking, &p->session)) {
        hmd->destroy(hmd); return false;
    }
    *device = hmd;
    return true;
}
