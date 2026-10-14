#include "pico-camera-pipeline.h"
#include <errno.h>
#include <stdlib.h>

enum { IFE, PHY, SENSOR };
struct pico_camera_pipeline {
    struct pico_camera_pipeline_config config;
    bool started, stopping;
    bool links[4], devices[3][4];
    int stop_error;
};
static struct pico_camera_pipeline_device device(const struct pico_camera_pipeline_config *c,
                                                 unsigned kind, size_t camera)
{
    return kind == IFE ? c->cameras[camera].ife :
           kind == PHY ? c->cameras[camera].phy : c->cameras[camera].sensor;
}
int pico_camera_pipeline_create(const struct pico_camera_pipeline_config *config,
                                 struct pico_camera_pipeline **pipeline)
{
    if (!config || !pipeline || *pipeline || config->crm_fd < 0 || config->session <= 0 ||
        !config->count || config->count > 4) return -EINVAL;
    int32_t handles[12]; size_t count = 0;
    for (size_t camera = 0; camera < config->count; ++camera) {
        if (config->cameras[camera].link <= 0) return -EINVAL;
        for (size_t j = 0; j < camera; ++j)
            if (config->cameras[j].link == config->cameras[camera].link) return -EINVAL;
        for (unsigned kind = 0; kind < 3; ++kind) {
            struct pico_camera_pipeline_device d = device(config, kind, camera);
            if (d.fd < 0 || d.handle <= 0) return -EINVAL;
            for (size_t i = 0; i < count; ++i) if (handles[i] == d.handle) return -EINVAL;
            handles[count++] = d.handle;
        }
    }
    struct pico_camera_pipeline *p = calloc(1, sizeof(*p));
    if (!p) return -ENOMEM;
    p->config = *config; *pipeline = p;
    return 0;
}
int pico_camera_pipeline_start(struct pico_camera_pipeline *p)
{
    if (!p || p->started || p->stopping) return -EINVAL;
    p->started = true;
    const struct pico_camera_pipeline_config *c = &p->config;
    for (size_t camera = 0; camera < c->count; ++camera) {
        p->links[camera] = true;
        int result = pico_camera_link_activate(c->crm_fd, c->session, c->cameras[camera].link, true);
        if (result) return result;
    }
    for (unsigned kind = IFE; kind <= SENSOR; ++kind) {
        for (size_t camera = 0; camera < c->count; ++camera) {
            struct pico_camera_pipeline_device d = device(c, kind, camera);
            p->devices[kind][camera] = true;
            int result = pico_camera_device_stream(d.fd, c->session, d.handle, true);
            if (result) return result;
        }
    }
    return 0;
}
int pico_camera_pipeline_stop(struct pico_camera_pipeline *p)
{
    if (!p) return -EINVAL;
    if (p->stopping) return p->stop_error;
    p->stopping = true;
    const struct pico_camera_pipeline_config *c = &p->config;
    const unsigned order[] = {IFE, SENSOR, PHY};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned kind = order[i];
        for (size_t camera = 0; camera < c->count; ++camera) {
            if (!p->devices[kind][camera]) continue;
            struct pico_camera_pipeline_device d = device(c, kind, camera);
            int result = pico_camera_device_stream(d.fd, c->session, d.handle, false);
            if (result && !p->stop_error) p->stop_error = result;
            if (!result) p->devices[kind][camera] = false;
        }
    }
    for (size_t camera = 0; camera < c->count; ++camera) {
        if (!p->links[camera]) continue;
        int result = pico_camera_link_activate(c->crm_fd, c->session, c->cameras[camera].link, false);
        if (result && !p->stop_error) p->stop_error = result;
        if (!result) p->links[camera] = false;
    }
    return p->stop_error;
}
int pico_camera_pipeline_destroy(struct pico_camera_pipeline **pipeline)
{
    if (!pipeline) return -EINVAL;
    if (!*pipeline) return 0;
    struct pico_camera_pipeline *p = *pipeline;
    if ((p->started && !p->stopping) || p->stop_error) return -EBUSY;
    free(p); *pipeline = NULL;
    return 0;
}
