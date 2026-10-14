#include "pico-slam-input.h"
#include <xrt/xrt_frame.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct pico_slam_input {
    struct pico_slam_input_config config;
    struct xrt_slam_sinks sinks;
    struct xrt_frame *pending[4];
    int64_t last_camera[4], first_imu, first_imu_uncertainty, last_imu, last_imu_uncertainty;
    uint64_t last_sequence[4], generation;
    int64_t offset, uncertainty, expires, last_now;
    bool active;
};

static bool fresh(int64_t time, int64_t uncertainty, int64_t now, int64_t age)
{
    return time > 0 && time < INT64_MAX && uncertainty >= 0 && uncertainty <= age &&
           now >= time && now-time >= uncertainty && now-time <= age-uncertainty;
}

static void discard(struct pico_slam_input *s)
{
    for (size_t i=0; i<s->config.count; ++i) xrt_frame_reference(&s->pending[i], NULL);
}

void pico_slam_input_stop(struct pico_slam_input *s)
{
    if (!s) return;
    s->active=false;
    discard(s);
}

bool pico_slam_input_create(const struct pico_slam_input_config *c,
                            const struct xrt_slam_sinks *sinks, struct pico_slam_input **input)
{
    if (!c || !sinks || !input || *input || (c->count!=2 && c->count!=4) ||
        c->count > XRT_TRACKING_MAX_CAMS || sinks->cam_count!=(int)c->count ||
        !sinks->imu || !sinks->imu->push_imu || c->max_age_ns<=0 || c->max_age_ns>1000000000 ||
        c->max_gap_ns<=0 || c->max_gap_ns>c->max_age_ns ||
        c->max_skew_ns<0 || c->max_skew_ns>PICO_CAMERA_MAX_SKEW_NS) return false;
    for (size_t i=0; i<c->count; ++i) {
        if (!sinks->cams[i] || !sinks->cams[i]->push_frame || !c->width[i] || c->width[i]>4096 ||
            !c->height[i] || c->height[i]>4096) return false;
        for (size_t j=0; j<i; ++j) if (c->camera_ids[i]==c->camera_ids[j]) return false;
    }
    struct pico_slam_input *s=calloc(1,sizeof(*s));
    if (!s) return false;
    s->config=*c; s->sinks=*sinks; *input=s;
    return true;
}

bool pico_slam_input_begin(struct pico_slam_input *s, uint64_t generation,
                           int64_t offset, int64_t uncertainty, int64_t expires)
{
    if (!s || !generation || generation<=s->generation || uncertainty<0 ||
        uncertainty>s->config.max_skew_ns/2 || uncertainty>s->config.max_age_ns || expires<=0)
        return false;
    discard(s);
    memset(s->last_camera,0,sizeof(s->last_camera));
    memset(s->last_sequence,0,sizeof(s->last_sequence));
    s->first_imu=0; s->first_imu_uncertainty=0; s->last_imu=0; s->last_imu_uncertainty=0;
    s->generation=generation; s->offset=offset; s->uncertainty=uncertainty;
    s->expires=expires; s->last_now=0; s->active=true;
    return true;
}

static bool current(struct pico_slam_input *s, uint64_t generation, int64_t now)
{
    if (!s || !s->active || generation!=s->generation || now<=0) return false;
    if (now>s->expires || now<s->last_now) {pico_slam_input_stop(s); return false;}
    s->last_now=now;
    return true;
}

static void submit(struct pico_slam_input *s, int64_t now)
{
    int64_t first=INT64_MAX,last=0;
    for (size_t i=0; i<s->config.count; ++i) {
        struct xrt_frame *f=s->pending[i];
        if (!f) return;
        if (!fresh(f->timestamp,s->uncertainty,now,s->config.max_age_ns)) {
            xrt_frame_reference(&s->pending[i],NULL); return;
        }
        if (f->timestamp<first) first=f->timestamp;
        if (f->timestamp>last) last=f->timestamp;
    }
    if (last-first > s->config.max_skew_ns-2*s->uncertainty) {
        for (size_t i=0; i<s->config.count; ++i)
            if (last-s->pending[i]->timestamp>s->config.max_skew_ns-2*s->uncertainty)
                xrt_frame_reference(&s->pending[i],NULL);
        return;
    }
    if (!s->first_imu || s->last_imu<last ||
        s->last_imu-last<s->last_imu_uncertainty+s->uncertainty) return;
    if (s->first_imu>first || first-s->first_imu<s->first_imu_uncertainty+s->uncertainty || !fresh(s->last_imu,s->last_imu_uncertainty,now,s->config.max_age_ns)) {
        discard(s); return;
    }
    /* Basalt's VIT interface requires ordered cameras with identical times. */
    for (size_t i=0; i<s->config.count; ++i) {
        s->pending[i]->timestamp=first;
        xrt_sink_push_frame(s->sinks.cams[i],s->pending[i]);
    }
    discard(s);
}

static void destroy_frame(struct xrt_frame *f) { free(f); }

static bool accept_camera(struct pico_slam_input *s, const struct pico_slam_mono8_frame *view, int64_t now)
{
    if (!view || !view->pixels || !view->width || view->width > 4096 || !view->height ||
        view->height > 4096 || view->stride < view->width || view->stride > SIZE_MAX / view->height ||
        view->size < (view->height - 1) * view->stride + view->width || view->center_ns <= 0) return false;
    struct pico_slam_mono8_frame image = *view;
    size_t index=0;
    while (index<s->config.count && s->config.camera_ids[index]!=image.camera_id) ++index;
    if (index==s->config.count || image.width!=s->config.width[index] || image.height!=s->config.height[index] ||
        (s->offset>0 && image.center_ns>INT64_MAX-s->offset) ||
        (s->offset<0 && s->offset < -image.center_ns)) return false;
    int64_t time=image.center_ns+s->offset;
    if (!fresh(time,s->uncertainty,now,s->config.max_age_ns) || time<=s->last_camera[index] ||
        (s->last_camera[index] && image.frame_number<=s->last_sequence[index])) return false;
    if (s->last_camera[index] && time-s->last_camera[index]>s->config.max_gap_ns) {
        pico_slam_input_stop(s); return false;
    }
    size_t pixel_size=(size_t)image.width*image.height;
    struct xrt_frame *f=calloc(1,sizeof(*f)+pixel_size);
    if (!f) return false;
    f->reference.count=1; f->destroy=destroy_frame;
    f->width=image.width; f->height=image.height; f->stride=image.width; f->size=pixel_size;
    f->data=(uint8_t *)(f+1);
    for (uint32_t row=0; row<image.height; ++row)
        memcpy(f->data+(size_t)row*image.width,image.pixels+(size_t)row*image.stride,image.width);
    f->format=XRT_FORMAT_L8; f->stereo_format=XRT_STEREO_FORMAT_NONE;
    f->timestamp=time; f->source_timestamp=image.center_ns;
    f->source_sequence=image.frame_number; f->source_id=image.camera_id;
    xrt_frame_reference(&s->pending[index],NULL); s->pending[index]=f;
    s->last_camera[index]=time; s->last_sequence[index]=image.frame_number;
    submit(s,now);
    return true;
}

bool pico_slam_input_mono8(struct pico_slam_input *s, uint64_t generation,
                           const struct pico_slam_mono8_frame *image, int64_t now)
{
    return current(s,generation,now) && accept_camera(s,image,now);
}

bool pico_slam_input_camera(struct pico_slam_input *s, uint64_t generation,
                            const void *snapshot, size_t size, int64_t now)
{
    if (!current(s,generation,now)) return false;
    struct pico_camera_frame image;
    if (!pico_camera_decode(snapshot,size,&image) || image.classification!=1 ||
        !image.exposure_lines || image.exposure_lines>737 || image.line_ns<=0) return false;
    struct pico_slam_mono8_frame view = {image.camera_id, image.width, image.height,
        image.width, image.size, image.pixels, image.center_ns, image.frame_number};
    return accept_camera(s,&view,now);
}

bool pico_slam_input_imu(struct pico_slam_input *s, uint64_t generation,
                         const struct xrt_imu_sample *sample, int64_t uncertainty, int64_t now)
{
    if (!current(s,generation,now) || !sample ||
        !fresh(sample->timestamp_ns,uncertainty,now,s->config.max_age_ns) ||
        sample->timestamp_ns<=s->last_imu || !isfinite(sample->accel_m_s2.x) ||
        !isfinite(sample->accel_m_s2.y) || !isfinite(sample->accel_m_s2.z) ||
        !isfinite(sample->gyro_rad_secs.x) || !isfinite(sample->gyro_rad_secs.y) ||
        !isfinite(sample->gyro_rad_secs.z)) return false;
    if (s->last_imu && sample->timestamp_ns-s->last_imu>s->config.max_gap_ns) {
        pico_slam_input_stop(s); return false;
    }
    if (!s->first_imu) {s->first_imu=sample->timestamp_ns; s->first_imu_uncertainty=uncertainty;}
    s->last_imu=sample->timestamp_ns; s->last_imu_uncertainty=uncertainty;
    struct xrt_imu_sample value=*sample;
    xrt_sink_push_imu(s->sinks.imu,&value);
    submit(s,now);
    return true;
}

void pico_slam_input_destroy(struct pico_slam_input **input)
{
    if (!input || !*input) return;
    pico_slam_input_stop(*input); free(*input); *input=NULL;
}
