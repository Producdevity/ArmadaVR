#include "pico-camera-rig.h"
#include "pico-camera-pipeline.h"
#include "pico-camera-ife.h"
#include <errno.h>
#include <stdlib.h>

enum { SENSOR, IFE, PHY, STAGES };
struct pico_camera_rig {
    struct pico_camera_session *session;
    struct pico_camera_session_view view;
    struct pico_camera_sensor_setup *sensors[2];
    struct pico_camera_front_end *ife[2], *phy[2];
    struct pico_camera_pipeline *pipeline;
    bool submitted[STAGES][2], ready, started, stopped, quiesced, session_quiesced;
    int stop_error;
};
static bool valid(const struct pico_camera_rig_config *c)
{
    if (!c || (c->command_mapping != PICO_CAMERA_MEMORY_IO && c->command_mapping != PICO_CAMERA_MEMORY_SHARED))
        return false;
    for (unsigned i = 0; i < 2; ++i) {
        const struct pico_camera_ife_config *input = &c->session.cameras[i].input;
        const struct pico_camera_csiphy_config *phy = &c->cameras[i].phy;
        uint8_t resource[PICO_CAMERA_IFE_ACQUIRE_SIZE], command[PICO_CAMERA_CSIPHY_SIZE];
        uint8_t clocks[PICO_CAMERA_CLOCK_BLOB_SIZE], bandwidth[PICO_CAMERA_BANDWIDTH_BLOB_SIZE];
        uint8_t throttle[PICO_CAMERA_IFE_THROTTLE_COMMAND_SIZE];
        if (input->bits != 8 || input->output != PICO_CAMERA_RAW_MIPI ||
            !pico_camera_ife_acquire(input, resource, sizeof(resource)) ||
            phy->cphy != input->cphy || phy->lanes != input->lanes || phy->lane_map != input->lane_map || phy->combo ||
            !pico_camera_csiphy_configure(phy, command, sizeof(command)) ||
            c->cameras[i].sample_pattern > 7 || c->cameras[i].clocks.dual ||
            !pico_camera_ife_clock_blob(&c->cameras[i].clocks, clocks, sizeof(clocks)) ||
            !pico_camera_ife_bandwidth_blob(&c->cameras[i].bandwidth, bandwidth, sizeof(bandwidth)) ||
            (c->cameras[i].configure_throttle &&
             !pico_camera_ife_throttle_command(c->cameras[i].utilization, throttle, sizeof(throttle)))) return false;
        const struct pico_camera_sensor_table *tables[] = {&c->cameras[i].initialize, &c->cameras[i].mode,
            &c->cameras[i].stream_on, &c->cameras[i].stream_off, &c->cameras[i].synchronize};
        for (unsigned j = 0; j < 5; ++j) {
            if (j == 4 && !tables[j]->count) continue;
            if (!pico_camera_sensor_writes_size(&tables[j]->bus, tables[j]->writes, tables[j]->count)) return false;
        }
    }
    return true;
}
int pico_camera_rig_prepare(const struct pico_camera_rig_config *c,
                               struct pico_camera_rig **device)
{
    if (!device || *device || !valid(c)) return -EINVAL;
    struct pico_camera_rig *d = calloc(1, sizeof(*d));
    if (!d) return -ENOMEM;
    *device = d;
    int result = pico_camera_session_acquire(&c->session, &d->session);
    if (result) return result;
    if (!pico_camera_session_view(d->session, &d->view)) return -EINVAL;
    struct pico_camera_pipeline_config pipeline = {.crm_fd = c->session.crm_fd,
        .session = d->view.session, .count = 2};
    for (unsigned i = 0; i < 2; ++i) {
        struct pico_camera_sensor_setup_config sensor = {.crm_fd = c->session.crm_fd,
            .sensor_fd = c->session.cameras[i].sensor_fd, .session = d->view.session, .sensor = d->view.cameras[i].sensor,
            .initialize = c->cameras[i].initialize, .mode = c->cameras[i].mode,
            .synchronize = c->cameras[i].synchronize, .stream_on = c->cameras[i].stream_on, .stream_off = c->cameras[i].stream_off};
        result = pico_camera_sensor_setup_prepare(&sensor, &d->sensors[i]);
        if (result) return result;
        struct pico_camera_ife_setup ife = {
            .target = {c->session.crm_fd, c->session.ife_fd, d->view.session, d->view.cameras[i].ife},
            .hardware = d->view.cameras[i].hardware, .command_mmu = d->view.capabilities.cdm_mmu,
            .command_mapping = c->command_mapping, .sample_pattern = c->cameras[i].sample_pattern,
            .clocks = c->cameras[i].clocks, .bandwidth = c->cameras[i].bandwidth,
            .configure_throttle = c->cameras[i].configure_throttle, .utilization = c->cameras[i].utilization};
        result = pico_camera_front_end_prepare_ife(&ife, &d->ife[i]);
        if (result) return result;
        struct pico_camera_front_end_target phy = {c->session.crm_fd, c->session.cameras[i].phy_fd,
            d->view.session, d->view.cameras[i].phy};
        result = pico_camera_front_end_prepare_phy(&phy, &c->cameras[i].phy, &d->phy[i]);
        if (result) return result;
        pipeline.cameras[i].link = d->view.cameras[i].link;
        pipeline.cameras[i].ife = (struct pico_camera_pipeline_device){c->session.ife_fd, d->view.cameras[i].ife};
        pipeline.cameras[i].phy = (struct pico_camera_pipeline_device){c->session.cameras[i].phy_fd, d->view.cameras[i].phy};
        pipeline.cameras[i].sensor = (struct pico_camera_pipeline_device){c->session.cameras[i].sensor_fd, d->view.cameras[i].sensor};
    }
    result = pico_camera_pipeline_create(&pipeline, &d->pipeline);
    if (result) return result;
    for (unsigned stage = SENSOR; stage < STAGES; ++stage) for (unsigned i = 0; i < 2; ++i) {
        d->submitted[stage][i] = true;
        result = stage == SENSOR ? pico_camera_sensor_setup_submit(d->sensors[i]) :
            pico_camera_front_end_submit(stage == IFE ? d->ife[i] : d->phy[i]);
        if (result) return result;
    }
    d->ready = true;
    return 0;
}
bool pico_camera_rig_view(const struct pico_camera_rig *d, struct pico_camera_session_view *view)
{
    if (!d || !d->ready || d->stopped || !view) return false;
    *view = d->view;
    return true;
}
int pico_camera_rig_start(struct pico_camera_rig *d)
{
    if (!d || !d->ready || d->started || d->stopped) return -EINVAL;
    d->started = true;
    int result = pico_camera_pipeline_start(d->pipeline);
    if (result) d->ready = false;
    return result;
}
int pico_camera_rig_stop(struct pico_camera_rig *d)
{
    if (!d) return -EINVAL;
    if (d->stopped) return d->stop_error;
    d->stopped = true; d->ready = false;
    d->stop_error = d->pipeline ? pico_camera_pipeline_stop(d->pipeline) : 0;
    return d->stop_error;
}
int pico_camera_rig_quiesced(struct pico_camera_rig *d)
{
    if (!d || !d->stopped || d->quiesced) return -EINVAL;
    if (d->stop_error) return d->stop_error;
    for (unsigned stage = SENSOR; stage < STAGES; ++stage) for (unsigned i = 0; i < 2; ++i) {
        if (!d->submitted[stage][i]) continue;
        int result = stage == SENSOR ? pico_camera_sensor_setup_quiesced(d->sensors[i]) :
            pico_camera_front_end_quiesced(stage == IFE ? d->ife[i] : d->phy[i]);
        if (result) return result;
        d->submitted[stage][i] = false;
    }
    d->quiesced = true;
    return 0;
}
int pico_camera_rig_release(struct pico_camera_rig **device)
{
    if (!device) return -EINVAL;
    struct pico_camera_rig *d = *device;
    if (!d) return 0;
    if (!d->quiesced) return -EBUSY;
    int result = pico_camera_pipeline_destroy(&d->pipeline);
    if (result) return result;
    for (unsigned i = 0; i < 2; ++i) {
        result = pico_camera_front_end_release(&d->ife[i]);
        if (!result) result = pico_camera_front_end_release(&d->phy[i]);
        if (!result) result = pico_camera_sensor_setup_release(&d->sensors[i]);
        if (result) return result;
    }
    if (d->session && !d->session_quiesced) {
        result = pico_camera_session_quiesced(d->session);
        if (result) return result;
        d->session_quiesced = true;
    }
    result = pico_camera_session_release(&d->session);
    if (result) return result;
    free(d); *device = NULL;
    return 0;
}
