#include "pico-monado-hmd.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned starts, stops, joins, live, checks;
static bool startup_failure, available;
struct pico_head_session { unsigned unused; };
bool pico_head_session_create(const struct ssc_imu_session_config *s,
                              const struct pico_head_tracking_config *t, struct pico_head_session **out)
{
    (void)s; (void)t; starts++;
    if (startup_failure) return false;
    *out = calloc(1, sizeof(**out)); assert(*out); live++; return true;
}
void pico_head_session_stop(struct pico_head_session *s) { if (s) stops++; }
bool pico_head_session_destroy(struct pico_head_session **s)
{
    if (*s) { assert(stops > joins); free(*s); *s = NULL; live--; joins++; }
    return true;
}
bool pico_head_session_get(struct pico_head_session *s, int64_t now, struct xrt_space_relation *r, int64_t *time)
{
    assert(s && now > 0); memset(r, 0, sizeof(*r)); r->pose.orientation.w = 1; *time = 0;
    if (!available) return false;
    r->pose.orientation.y = r->pose.orientation.w = sqrtf(0.5f);
    r->relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;
    *time = now - 1000; return true;
}

static struct pico_hmd_config configuration(struct xrt_uv_triplet *grid, unsigned n)
{
    struct pico_hmd_config c = {.serial="offline-fixture", .screen_width=1920, .screen_height=3664,
                               .frame_interval_ns=11111111};
    for (unsigned i=0;i<2;i++) {
        c.views[i] = (struct pico_hmd_view){
            .display={.viewport={0,i*1832,1920,1832},.display={1832,1920},.rot={.v={0,1,-1,0}}},
            .fov={.angle_left=-0.8f,.angle_right=0.9f,.angle_up=0.7f,.angle_down=-0.6f},
            .eye_pose={.orientation={0,0,0,1},.position={i?0.032f:-0.032f,0,0}},
            .grid_width=n,.grid_height=n,.distortion=grid};
    }
    return c;
}
static void reject(struct pico_hmd_config *c)
{
    unsigned prior=starts;struct xrt_device *d=NULL;
    assert(!pico_monado_hmd_create(c,&d) && !d && starts==prior);checks++;
}
int main(void)
{
    for (unsigned n=2;n<=65;n++) {
        struct xrt_uv_triplet *grid=calloc(n*n,sizeof(*grid));assert(grid);
        for(unsigned y=0;y<n;y++)for(unsigned x=0;x<n;x++) {
            float u=(float)x/(n-1),v=(float)y/(n-1);
            grid[y*n+x]=(struct xrt_uv_triplet){.r={u,v},.g={0.1f+0.8f*u,v},.b={u,0.2f+0.6f*v}};
        }
        struct pico_hmd_config c=configuration(grid,n);
        struct xrt_device *d=NULL;assert(pico_monado_hmd_create(&c,&d));
        assert(d->device_type==XRT_DEVICE_TYPE_HMD && d->hmd->view_count==2);
        assert(d->supported.orientation_tracking && !d->supported.position_tracking);
        assert(d->hmd->screens[0].h_pixels==3664 && d->hmd->views[1].viewport.y_pixels==1832);
        assert(d->hmd->distortion.models==XRT_DISTORTION_MODEL_COMPUTE && d->id.val);
        memset(grid,0,n*n*sizeof(*grid));free(grid);
        for (unsigned eye=0;eye<2;eye++) for(unsigned y=0;y<=16;y++)for(unsigned x=0;x<=16;x++) {
            float u=x/16.0f,v=y/16.0f;struct xrt_uv_triplet out;
            assert(d->compute_distortion(d,eye,u,v,&out)==XRT_SUCCESS);
            assert(fabsf(out.r.x-u)<0.00001f && fabsf(out.r.y-v)<0.00001f);
            assert(fabsf(out.g.x-(0.1f+0.8f*u))<0.00001f && fabsf(out.g.y-v)<0.00001f);
            assert(fabsf(out.b.x-u)<0.00001f && fabsf(out.b.y-(0.2f+0.6f*v))<0.00001f);checks++;
        }
        struct xrt_uv_triplet sentinel={.r={9,9},.g={9,9},.b={9,9}},out=sentinel;
        assert(d->compute_distortion(d,2,0,0,&out)==XRT_ERROR_INVALID_ARGUMENT && !memcmp(&out,&sentinel,sizeof(out)));
        const float bad[]={NAN,INFINITY,-1,1.001f};
        for(unsigned i=0;i<4;i++) {
            assert(d->compute_distortion(d,0,bad[i],0,&out)==XRT_ERROR_INVALID_ARGUMENT);
            assert(d->compute_distortion(d,0,0,bad[i],&out)==XRT_ERROR_INVALID_ARGUMENT);
        }
        struct xrt_space_relation r;struct xrt_fov f[2];struct xrt_pose p[2];struct xrt_vec3 ignored={1,2,3};
        available=true;
        assert(d->update_inputs(d)==XRT_SUCCESS && d->inputs[0].active && d->inputs[0].timestamp>0);
        assert(d->get_view_poses(d,&ignored,INT64_MAX,XRT_VIEW_TYPE_STEREO,2,&r,f,p)==XRT_SUCCESS);
        assert(r.relation_flags==(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT|XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT));
        assert(fabsf(r.pose.orientation.y-sqrtf(0.5f))<0.00001f && r.pose.position.x==0);
        assert(!memcmp(f,&c.views[0].fov,sizeof(f[0])) && p[0].position.x==-0.032f && p[1].position.x==0.032f);
        assert(d->get_view_poses(d,&ignored,0,XRT_VIEW_TYPE_STEREO,1,&r,f,p)==XRT_ERROR_INVALID_ARGUMENT);
        available=false;
        assert(d->update_inputs(d)==XRT_SUCCESS && !d->inputs[0].active && !d->inputs[0].timestamp);
        assert(d->get_tracked_pose(d,XRT_INPUT_GENERIC_HEAD_POSE,0,&r)==XRT_SUCCESS && !r.relation_flags);
        assert(d->get_tracked_pose(d,XRT_INPUT_PICO_NEO3_GRIP_POSE,0,&r)==XRT_ERROR_INPUT_UNSUPPORTED);
        d->destroy(d);assert(!live && stops==joins);
    }
    struct xrt_uv_triplet grid[4]={0};struct pico_hmd_config original=configuration(grid,2),c;
#define BAD(expression) do { c=original; expression; reject(&c); } while(0)
    reject(NULL);
    BAD(c.serial=NULL);BAD(c.serial="");BAD(c.screen_width=0);BAD(c.screen_height=16385);
    BAD(c.frame_interval_ns=0);BAD(c.frame_interval_ns=100000001);
    BAD(c.views[0].grid_width=1);BAD(c.views[0].grid_height=66);BAD(c.views[1].distortion=NULL);
    BAD(c.views[0].display.viewport.x_pixels=UINT32_MAX);BAD(c.views[1].display.viewport.y_pixels=1);
    BAD(c.views[0].display.viewport.w_pixels=0);BAD(c.views[0].display.display.w_pixels=0);
    BAD(c.views[0].display.rot.v[0]=NAN);BAD(c.views[0].display.rot.v[1]=-1);
    BAD(c.views[1].fov.angle_left=0);BAD(c.views[0].fov.angle_up=INFINITY);
    BAD(c.views[0].eye_pose.orientation.w=0);BAD(c.views[0].eye_pose.position.x=0.3f);
    BAD(c.views[1].eye_pose.position.y=NAN);
    grid[0].r.x=NAN;reject(&original);grid[0].r.x=0;
    startup_failure=true;struct xrt_device *d=NULL;
    assert(!pico_monado_hmd_create(&original,&d) && !d && !live);
    startup_failure=false;
    for(unsigned i=0;i<1000;i++){assert(pico_monado_hmd_create(&original,&d));d->destroy(d);d=NULL;}
    assert(!live && stops==joins && joins==1064);
    printf("PICO_MONADO_HMD_PASS checks=%u joined=%u\n",checks,joins);
    return 0;
}
