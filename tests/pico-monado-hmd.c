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

bool pico_head_session_get_at(struct pico_head_session *s, int64_t now, int64_t at,
                              struct xrt_space_relation *r)
{
    int64_t sample;
    bool valid=pico_head_session_get(s,now,r,&sample);
    if(at!=1234567890) {memset(r,0,sizeof(*r));return false;}
    return valid;
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
static void radial_tests(void)
{
    struct pico_hmd_config base=configuration(NULL,0);
    for(unsigned eye=0;eye<2;eye++) {
        base.views[eye].radial_enabled=true;
        base.views[eye].radial=(struct pico_hmd_radial){.tangent_scale={2,-2},.tangent_offset={-1,1},
          .radial={1,.1,.02},.chromatic={.02,-.01,-.03,.02,.04,-.02}};
    }
    unsigned samples=0;
    for(unsigned reverse=0;reverse<2;reverse++) {
        struct pico_hmd_config c=base;
        if(reverse) for(unsigned eye=0;eye<2;eye++)for(unsigned i=0;i<2;i++) {
            c.views[eye].radial.tangent_scale[i]*=-1;c.views[eye].radial.tangent_offset[i]*=-1;
        }
        struct xrt_device *device=NULL;assert(pico_monado_hmd_create(&c,&device));
        struct pico_hmd_config saved=c;memset(&c,0,sizeof(c));
        for(unsigned eye=0;eye<2;eye++)for(unsigned row=0;row<=48;row++)for(unsigned col=0;col<=64;col++) {
            float u=(float)col/64,v=(float)row/48;struct xrt_uv_triplet actual;
            assert(device->compute_distortion(device,eye,u,v,&actual)==XRT_SUCCESS);
            const struct pico_hmd_radial *r=&saved.views[eye].radial;
            long double x=(long double)r->tangent_scale[0]*u+r->tangent_offset[0];
            long double y=(long double)r->tangent_scale[1]*v+r->tangent_offset[1], rr=x*x+y*y;
            long double scale=0;
            for(unsigned i=0;i<11;i++)scale+=r->radial[i]*powl(rr,i);
            long double factors[]={1+(long double)r->chromatic[0]+r->chromatic[1]*rr+r->chromatic[4]*sqrtl(rr),1,
              1+(long double)r->chromatic[2]+r->chromatic[3]*rr+r->chromatic[5]*sqrtl(rr)};
            struct xrt_fov f=saved.views[eye].fov;
            long double left=tanl(f.angle_left),right=tanl(f.angle_right),up=tanl(f.angle_up),down=tanl(f.angle_down);
            struct xrt_vec2 channels[]={actual.r,actual.g,actual.b};
            for(unsigned color=0;color<3;color++) {
                long double wantx=(x*scale*factors[color]-left)/(right-left),wanty=(up-y*scale*factors[color])/(up-down);
                assert(fabsl(channels[color].x-wantx)<1e-6L && fabsl(channels[color].y-wanty)<1e-6L);
            }
            samples++;
        }
        device->destroy(device);
    }
    struct pico_hmd_config c;
#define BAD_RADIAL(expression) do {c=base;expression;reject(&c);}while(0)
    BAD_RADIAL(c.views[0].grid_width=2);BAD_RADIAL(c.views[0].distortion=(void *)1);
    BAD_RADIAL(c.views[0].radial.tangent_scale[0]=0);BAD_RADIAL(c.views[0].radial.tangent_offset[1]=INFINITY);
    BAD_RADIAL(c.views[0].radial.radial[2]=NAN);BAD_RADIAL(c.views[0].radial.chromatic[5]=NAN);
    BAD_RADIAL(c.views[0].radial.radial[10]=1000000);
    assert(!live && stops==joins);
    printf("PICO_HMD_RADIAL_PASS samples=%u\n",samples);
}

static int radial_samples(void)
{
    unsigned count;
    while(scanf("%u",&count)==1) {
        assert(count>0&&count<=10000);struct pico_hmd_config c=configuration(NULL,0);
        struct pico_hmd_radial r={0};float f[4];
        for(unsigned i=0;i<2;i++)assert(scanf("%f",&r.tangent_scale[i])==1);
        for(unsigned i=0;i<2;i++)assert(scanf("%f",&r.tangent_offset[i])==1);
        for(unsigned i=0;i<11;i++)assert(scanf("%f",&r.radial[i])==1);
        for(unsigned i=0;i<6;i++)assert(scanf("%f",&r.chromatic[i])==1);
        for(unsigned i=0;i<4;i++)assert(scanf("%f",&f[i])==1);
        for(unsigned eye=0;eye<2;eye++) {
            c.views[eye].radial_enabled=true;c.views[eye].radial=r;
            c.views[eye].fov=(struct xrt_fov){.angle_left=f[0],.angle_right=f[1],.angle_up=f[2],.angle_down=f[3]};
        }
        struct xrt_device *d=NULL;assert(pico_monado_hmd_create(&c,&d));
        for(unsigned i=0;i<count;i++) {
            float u,v;assert(scanf("%f %f",&u,&v)==2);struct xrt_uv_triplet out;
            assert(d->compute_distortion(d,0,u,v,&out)==XRT_SUCCESS);
            printf("%a %a %a %a %a %a\n",(double)out.r.x,(double)out.r.y,(double)out.g.x,(double)out.g.y,(double)out.b.x,(double)out.b.y);
        }
        d->destroy(d);
    }
    assert(!live&&stops==joins);return 0;
}

int main(int argc, char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--radial-samples")) return radial_samples();
    assert(argc==1);
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
        assert(d->get_view_poses(d,&ignored,1234567890,XRT_VIEW_TYPE_STEREO,2,&r,f,p)==XRT_SUCCESS);
        assert(r.relation_flags==(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT|XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT));
        assert(fabsf(r.pose.orientation.y-sqrtf(0.5f))<0.00001f && r.pose.position.x==0);
        assert(!memcmp(f,&c.views[0].fov,sizeof(f[0])) && p[0].position.x==-0.032f && p[1].position.x==0.032f);
        assert(d->get_view_poses(d,&ignored,0,XRT_VIEW_TYPE_STEREO,1,&r,f,p)==XRT_ERROR_INVALID_ARGUMENT);
        assert(d->get_tracked_pose(d,XRT_INPUT_GENERIC_HEAD_POSE,INT64_MAX,&r)==XRT_SUCCESS && !r.relation_flags);
        assert(d->get_tracked_pose(d,XRT_INPUT_GENERIC_HEAD_POSE,0,&r)==XRT_SUCCESS && !r.relation_flags);
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
    radial_tests();
    return 0;
}
