#include "pico-monado-hmd.h"

#include <os/os_time.h>
#include <util/u_device.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pico_hmd {
    struct xrt_device base;
    struct xrt_hmd_parts parts;
    struct xrt_tracking_origin origin;
    struct xrt_input input;
    struct pico_head_session *session;
    struct pico_slam_tracking *visual_tracking;
    struct xrt_pose eyes[2];
    unsigned width[2], height[2];
    struct xrt_uv_triplet *grids[2];
    bool radial_enabled[2];
    struct pico_hmd_radial radial[2];
};

static bool radial_valid(const struct pico_hmd_radial *r, const struct xrt_fov *fov)
{
    double extent[2];
    for (unsigned i = 0; i < 2; i++) {
        if (!isfinite(r->tangent_scale[i]) || !isfinite(r->tangent_offset[i]) ||
            fabsf(r->tangent_scale[i]) > 32 || fabsf(r->tangent_offset[i]) > 32 ||
            fabsf(r->tangent_scale[i]) < 0.000001f) return false;
        extent[i] = fmax(fabs(r->tangent_offset[i]), fabs((double)r->tangent_scale[i] + r->tangent_offset[i]));
    }
    double radius_squared = extent[0]*extent[0] + extent[1]*extent[1];
    double bound = 0;
    for (int i = 10; i >= 0; i--) {
        if (!isfinite(r->radial[i]) || fabsf(r->radial[i]) > 1000000) return false;
        bound = bound * radius_squared + fabs(r->radial[i]);
    }
    for (unsigned i = 0; i < 6; i++)
        if (!isfinite(r->chromatic[i]) || fabsf(r->chromatic[i]) > 1000000) return false;
    double red = 1 + fabs(r->chromatic[0]) + fabs(r->chromatic[1])*radius_squared + fabs(r->chromatic[4])*sqrt(radius_squared);
    double blue = 1 + fabs(r->chromatic[2]) + fabs(r->chromatic[3])*radius_squared + fabs(r->chromatic[5])*sqrt(radius_squared);
    bound *= fmax(1, fmax(red, blue));
    double left=tan(fov->angle_left), right=tan(fov->angle_right), up=tan(fov->angle_up), down=tan(fov->angle_down);
    return isfinite(bound) && (extent[0]*bound + fabs(left))/(right-left) <= 1000000 &&
           (extent[1]*bound + fabs(up))/(up-down) <= 1000000;
}

static xrt_result_t radial_distortion(const struct pico_hmd_radial *r, const struct xrt_fov *fov,
                                      float u, float v, struct xrt_uv_triplet *result)
{
    double x = (double)r->tangent_scale[0]*u + r->tangent_offset[0];
    double y = (double)r->tangent_scale[1]*v + r->tangent_offset[1];
    double radius_squared = x*x+y*y, radius = sqrt(radius_squared), scale = 0;
    for (int i=10;i>=0;i--) scale=scale*radius_squared+r->radial[i];
    double factors[] = {1+r->chromatic[0]+radius_squared*r->chromatic[1]+radius*r->chromatic[4],
                        1, 1+r->chromatic[2]+radius_squared*r->chromatic[3]+radius*r->chromatic[5]};
    struct xrt_uv_triplet value;
    struct xrt_vec2 *channels[] = {&value.r,&value.g,&value.b};
    double left=tan(fov->angle_left), right=tan(fov->angle_right), up=tan(fov->angle_up), down=tan(fov->angle_down);
    for (unsigned i=0;i<3;i++) {
        channels[i]->x=(float)((x*scale*factors[i]-left)/(right-left));
        channels[i]->y=(float)((up-y*scale*factors[i])/(up-down));
        if (!isfinite(channels[i]->x) || !isfinite(channels[i]->y)) return XRT_ERROR_INVALID_ARGUMENT;
    }
    *result=value;
    return XRT_SUCCESS;
}

static bool optics_valid(const struct pico_hmd_config *c)
{
    if (!c || !c->serial || !*c->serial || strnlen(c->serial, XRT_DEVICE_NAME_LEN) >= XRT_DEVICE_NAME_LEN ||
        c->screen_width == 0 || c->screen_width > 16384 || c->screen_height == 0 || c->screen_height > 16384 ||
        c->frame_interval_ns < 1000000 || c->frame_interval_ns > 100000000) return false;
    for (unsigned i = 0; i < 2; i++) {
        const struct pico_hmd_view *v = &c->views[i];
        const struct xrt_view *d = &v->display;
        if (!d->viewport.w_pixels || !d->viewport.h_pixels ||
            d->viewport.w_pixels > c->screen_width || d->viewport.h_pixels > c->screen_height ||
            d->viewport.x_pixels > c->screen_width - d->viewport.w_pixels ||
            d->viewport.y_pixels > c->screen_height - d->viewport.h_pixels ||
            !d->display.w_pixels || d->display.w_pixels > 16384 ||
            !d->display.h_pixels || d->display.h_pixels > 16384) return false;
        const float *r = d->rot.v;
        for (unsigned j = 0; j < 4; j++) if (!isfinite(r[j])) return false;
        if (fabsf(r[0]*r[0] + r[1]*r[1] - 1) > 0.0001f ||
            fabsf(r[2]*r[2] + r[3]*r[3] - 1) > 0.0001f ||
            fabsf(r[0]*r[2] + r[1]*r[3]) > 0.0001f ||
            fabsf(r[0]*r[3] - r[1]*r[2] - 1) > 0.0001f) return false;
        const struct xrt_fov *f = &v->fov;
        if (!isfinite(f->angle_left) || !isfinite(f->angle_right) ||
            !isfinite(f->angle_up) || !isfinite(f->angle_down) ||
            f->angle_left >= 0 || f->angle_left <= -1.57f || f->angle_right <= 0 || f->angle_right >= 1.57f ||
            f->angle_down >= 0 || f->angle_down <= -1.57f || f->angle_up <= 0 || f->angle_up >= 1.57f) return false;
        const struct xrt_quat q = v->eye_pose.orientation;
        const struct xrt_vec3 p = v->eye_pose.position;
        if (!isfinite(q.x) || !isfinite(q.y) || !isfinite(q.z) || !isfinite(q.w) ||
            fabsf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w - 1) > 0.0001f ||
            !isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z) ||
            fabsf(p.x) > 0.2f || fabsf(p.y) > 0.2f || fabsf(p.z) > 0.2f) return false;
        if (v->radial_enabled) {
            if (v->distortion || v->grid_width || v->grid_height || !radial_valid(&v->radial, f)) return false;
            continue;
        }
        if (!v->distortion || v->grid_width < 2 || v->grid_width > 65 ||
            v->grid_height < 2 || v->grid_height > 65) return false;
        for (unsigned j = 0; j < v->grid_width * v->grid_height; j++) {
            const struct xrt_uv_triplet *t = &v->distortion[j];
            const struct xrt_vec2 channels[] = {t->r, t->g, t->b};
            for (unsigned k = 0; k < 3; k++)
                if (!isfinite(channels[k].x) || !isfinite(channels[k].y) ||
                    fabsf(channels[k].x) > 4 || fabsf(channels[k].y) > 4) return false;
        }
    }
    const struct xrt_view *a = &c->views[0].display, *b = &c->views[1].display;
    return a->viewport.x_pixels + a->viewport.w_pixels <= b->viewport.x_pixels ||
           b->viewport.x_pixels + b->viewport.w_pixels <= a->viewport.x_pixels ||
           a->viewport.y_pixels + a->viewport.h_pixels <= b->viewport.y_pixels ||
           b->viewport.y_pixels + b->viewport.h_pixels <= a->viewport.y_pixels;
}

static xrt_result_t get_pose(struct xrt_device *xdev, enum xrt_input_name name,
                             int64_t at_timestamp_ns, struct xrt_space_relation *relation)
{
    if (!relation) return XRT_ERROR_INVALID_ARGUMENT;
    memset(relation, 0, sizeof(*relation));
    relation->pose.orientation.w = 1;
    if (name != XRT_INPUT_GENERIC_HEAD_POSE) return XRT_ERROR_INPUT_UNSUPPORTED;
    struct pico_hmd *hmd = (struct pico_hmd *)xdev;
    int64_t now = (int64_t)os_monotonic_get_ns();
    if (hmd->visual_tracking) {
        int64_t sample_time;
        pico_slam_tracking_get(hmd->visual_tracking, now, at_timestamp_ns, relation, &sample_time);
    } else {
        pico_head_session_get_at(hmd->session, now, at_timestamp_ns, relation);
    }
    return XRT_SUCCESS;
}

static xrt_result_t update_inputs(struct xrt_device *xdev)
{
    struct pico_hmd *hmd = (struct pico_hmd *)xdev;
    struct xrt_space_relation relation;
    int64_t time;
    int64_t now = (int64_t)os_monotonic_get_ns();
    hmd->input.active = hmd->visual_tracking ?
        pico_slam_tracking_get(hmd->visual_tracking, now, now, &relation, &time) :
        pico_head_session_get(hmd->session, now, &relation, &time);
    hmd->input.timestamp = time;
    return XRT_SUCCESS;
}

static xrt_result_t get_views(struct xrt_device *xdev, const struct xrt_vec3 *default_eye_relation,
                              int64_t at_timestamp_ns, enum xrt_view_type type, uint32_t count,
                              struct xrt_space_relation *head, struct xrt_fov *fovs, struct xrt_pose *poses)
{
    (void)default_eye_relation;
    if (type != XRT_VIEW_TYPE_STEREO || count != 2 || !head || !fovs || !poses) return XRT_ERROR_INVALID_ARGUMENT;
    struct pico_hmd *hmd = (struct pico_hmd *)xdev;
    xrt_result_t result = get_pose(xdev, XRT_INPUT_GENERIC_HEAD_POSE, at_timestamp_ns, head);
    for (unsigned i = 0; i < 2; i++) { fovs[i] = hmd->parts.distortion.fov[i]; poses[i] = hmd->eyes[i]; }
    return result;
}

static struct xrt_vec2 interpolate(struct xrt_vec2 a, struct xrt_vec2 b, float t)
{
    return (struct xrt_vec2){a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
}

static xrt_result_t distortion(struct xrt_device *xdev, uint32_t view, float u, float v,
                               struct xrt_uv_triplet *result)
{
    if (!result || view >= 2 || !isfinite(u) || !isfinite(v) || u < 0 || u > 1 || v < 0 || v > 1)
        return XRT_ERROR_INVALID_ARGUMENT;
    const struct pico_hmd *hmd = (const struct pico_hmd *)xdev;
    if (hmd->radial_enabled[view])
        return radial_distortion(&hmd->radial[view], &hmd->parts.distortion.fov[view], u, v, result);
    float x = u * (hmd->width[view] - 1), y = v * (hmd->height[view] - 1);
    unsigned col = fminf(floorf(x), hmd->width[view] - 2), row = fminf(floorf(y), hmd->height[view] - 2);
    const struct xrt_uv_triplet *a = &hmd->grids[view][row * hmd->width[view] + col];
    const struct xrt_uv_triplet *b = a + hmd->width[view];
    *result = (struct xrt_uv_triplet){
        .r = interpolate(interpolate(a[0].r, a[1].r, x-col), interpolate(b[0].r, b[1].r, x-col), y-row),
        .g = interpolate(interpolate(a[0].g, a[1].g, x-col), interpolate(b[0].g, b[1].g, x-col), y-row),
        .b = interpolate(interpolate(a[0].b, a[1].b, x-col), interpolate(b[0].b, b[1].b, x-col), y-row)};
    return XRT_SUCCESS;
}

static void destroy(struct xrt_device *xdev)
{
    struct pico_hmd *hmd = (struct pico_hmd *)xdev;
    pico_head_session_stop(hmd->session);
    if (!pico_head_session_destroy(&hmd->session)) return;
    free(hmd->grids[0]); free(hmd->grids[1]);
    u_device_free(xdev);
}

bool pico_monado_hmd_create(const struct pico_hmd_config *config, struct xrt_device **device)
{
    if (!device || *device || !optics_valid(config)) return false;
    struct pico_hmd *hmd = calloc(1, sizeof(*hmd));
    if (!hmd) return false;
    struct xrt_device *xdev = &hmd->base;
    u_device_populate_function_pointers(xdev, get_pose, destroy);
    xdev->hmd = &hmd->parts;
    xdev->tracking_origin = &hmd->origin;
    hmd->origin.type = XRT_TRACKING_TYPE_OTHER;
    hmd->origin.initial_offset.orientation.w = 1;
    snprintf(hmd->origin.name, sizeof(hmd->origin.name), "%s", config->visual_tracking ? "Pico visual-inertial tracking" : "Pico head orientation");
    xdev->name = XRT_DEVICE_GENERIC_HMD;
    xdev->device_type = XRT_DEVICE_TYPE_HMD;
    snprintf(xdev->str, sizeof(xdev->str), "Pico Neo3 HMD");
    snprintf(xdev->serial, sizeof(xdev->serial), "%s", config->serial);
    xdev->supported.orientation_tracking = true;
    xdev->supported.position_tracking = config->visual_tracking != NULL;
    hmd->visual_tracking = config->visual_tracking;
    xdev->input_count = 1; xdev->inputs = &hmd->input;
    hmd->input.name = XRT_INPUT_GENERIC_HEAD_POSE;
    xdev->update_inputs = update_inputs;
    xdev->get_view_poses = get_views;
    xdev->compute_distortion = distortion;
    hmd->parts.view_count = 2;
    hmd->parts.screens[0].w_pixels = (int)config->screen_width;
    hmd->parts.screens[0].h_pixels = (int)config->screen_height;
    hmd->parts.screens[0].nominal_frame_interval_ns = config->frame_interval_ns;
    hmd->parts.blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
    hmd->parts.blend_mode_count = 1;
    hmd->parts.distortion.models = hmd->parts.distortion.preferred = XRT_DISTORTION_MODEL_COMPUTE;
    for (unsigned i = 0; i < 2; i++) {
        const struct pico_hmd_view *v = &config->views[i];
        hmd->parts.views[i] = v->display; hmd->parts.distortion.fov[i] = v->fov;
        hmd->eyes[i] = v->eye_pose; hmd->width[i] = v->grid_width; hmd->height[i] = v->grid_height;
        hmd->radial_enabled[i] = v->radial_enabled; hmd->radial[i] = v->radial;
        if (v->radial_enabled) continue;
        size_t size = v->grid_width * v->grid_height * sizeof(*v->distortion);
        hmd->grids[i] = malloc(size);
        if (!hmd->grids[i]) { destroy(xdev); return false; }
        memcpy(hmd->grids[i], v->distortion, size);
    }
    if (!hmd->visual_tracking && !pico_head_session_create(&config->sensors, &config->tracking, &hmd->session)) {
        destroy(xdev); return false;
    }
    *device = xdev;
    return true;
}
