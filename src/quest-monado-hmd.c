#include "quest-monado-hmd.h"
#include "quest-slam-runtime.h"
#include <string.h>

static bool get_pose(void *userdata, int64_t now, int64_t at, struct xrt_space_relation *relation)
{
    int64_t sample;
    return quest_slam_runtime_get_relation(userdata, now, at, relation, &sample);
}

static bool update_input(void *userdata, int64_t now, struct xrt_space_relation *relation, int64_t *sample)
{
    return quest_slam_runtime_get_relation(userdata, now, now, relation, sample);
}

bool quest_monado_hmd_create(const struct quest_hmd_config *config, struct xrt_device **device)
{
    if (!config || !config->tracking || config->views[0].radial_enabled || config->views[1].radial_enabled)
        return false;
    struct calibrated_hmd_config optics = {
        .serial = config->serial, .name = "Quest 3 HMD", .tracking_name = "Quest visual-inertial tracking",
        .screen_width = config->screen_width, .screen_height = config->screen_height,
        .frame_interval_ns = config->frame_interval_ns, .position_tracking = true,
    };
    memcpy(optics.views, config->views, sizeof(optics.views));
    struct calibrated_hmd_provider provider = {config->tracking, get_pose, update_input, NULL};
    return calibrated_hmd_create(&optics, &provider, device);
}
