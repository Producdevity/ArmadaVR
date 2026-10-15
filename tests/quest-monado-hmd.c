#include "quest-monado-hmd.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

struct quest_slam_runtime { unsigned calls; bool available; int64_t last_at; };
bool quest_slam_runtime_get_relation(struct quest_slam_runtime *p, int64_t now, int64_t at,
                                     struct xrt_space_relation *relation, int64_t *sample)
{
    assert(p && now > 0 && at > 0); p->calls++; p->last_at = at;
    memset(relation, 0, sizeof(*relation)); relation->pose.orientation.w = 1;
    relation->relation_flags = XRT_SPACE_RELATION_POSITION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT;
    relation->pose.position.x = .25f; *sample = now - 1000;
    return p->available;
}

static struct quest_hmd_config configuration(struct quest_slam_runtime *p, struct xrt_uv_triplet *grid)
{
    struct quest_hmd_config c = {.serial="synthetic-optics", .screen_width=200, .screen_height=100,
                                 .frame_interval_ns=10000000, .tracking=p};
    for (unsigned i=0;i<2;i++) {
        c.views[i]=(struct calibrated_hmd_view){
            .display={.viewport={i*100,0,100,100},.display={100,100},.rot={.v={1,0,0,1}}},
            .fov={-.8f,.9f,.7f,-.6f}, .eye_pose={.orientation={0,0,0,1},.position={i?.03f:-.03f,0,0}},
            .grid_width=2,.grid_height=2,.distortion=grid};
    }
    return c;
}

int main(void)
{
    struct quest_slam_runtime runtime={0};
    struct xrt_uv_triplet grid[4];
    for (unsigned i=0;i<4;i++) grid[i]=(struct xrt_uv_triplet){
        .r={(i%2)*.8f+.1f,(i/2)*.6f+.2f},.g={(i%2)*.7f+.2f,(i/2)*.4f+.3f},
        .b={(i%2)*.5f+.3f,(i/2)*.9f+.05f}};
    struct quest_hmd_config c=configuration(&runtime,grid);
    struct xrt_device *d=NULL;
    assert(!quest_monado_hmd_create(NULL,&d));
    assert(!quest_monado_hmd_create(&c,NULL));
    c.tracking=NULL; assert(!quest_monado_hmd_create(&c,&d));c.tracking=&runtime;
    for (unsigned eye=0;eye<2;eye++) {
        c.views[eye].radial_enabled=true;assert(!quest_monado_hmd_create(&c,&d));c.views[eye].radial_enabled=false;
        c.views[eye].distortion=NULL;assert(!quest_monado_hmd_create(&c,&d));c.views[eye].distortion=grid;
    }
    assert(!d && !runtime.calls);
    unsigned samples=0;
    for (unsigned cycle=0;cycle<200;cycle++) {
        struct quest_hmd_config copy=c;
        struct xrt_uv_triplet pixels[4];memcpy(pixels,grid,sizeof(grid));
        copy.views[0].distortion=copy.views[1].distortion=pixels;
        unsigned calls=runtime.calls;
        assert(quest_monado_hmd_create(&copy,&d) && d && runtime.calls==calls);
        assert(!quest_monado_hmd_create(&copy,&d));
        memset(pixels,0xa5,sizeof(pixels));memset(&copy,0xa5,sizeof(copy));
        assert(!strcmp(d->str,"Quest 3 HMD") && !strcmp(d->serial,"synthetic-optics"));
        assert(d->supported.position_tracking && d->supported.orientation_tracking && d->hmd->view_count==2);
        runtime.available=true;
        assert(d->update_inputs(d)==XRT_SUCCESS && d->inputs[0].active && d->inputs[0].timestamp>0);
        struct xrt_space_relation pose;struct xrt_fov fovs[2];struct xrt_pose eyes[2];
        assert(d->get_tracked_pose(d,XRT_INPUT_GENERIC_HEAD_POSE,1234567890,&pose)==XRT_SUCCESS);
        assert(runtime.last_at==1234567890 && pose.pose.position.x==.25f && pose.relation_flags);
        assert(d->get_view_poses(d,NULL,1234567891,XRT_VIEW_TYPE_STEREO,2,&pose,fovs,eyes)==XRT_SUCCESS);
        assert(runtime.last_at==1234567891 && eyes[0].position.x==-.03f && eyes[1].position.x==.03f);
        assert(!memcmp(fovs,&c.views[0].fov,sizeof(fovs[0])));
        calls=runtime.calls;
        assert(d->get_tracked_pose(d,XRT_INPUT_GENERIC_HEAD_POSE,1,NULL)==XRT_ERROR_INVALID_ARGUMENT);
        assert(d->get_tracked_pose(d,XRT_INPUT_GENERIC_TRACKER_POSE,1,&pose)==XRT_ERROR_INPUT_UNSUPPORTED);
        assert(d->get_view_poses(d,NULL,1,XRT_VIEW_TYPE_STEREO,1,&pose,fovs,eyes)==XRT_ERROR_INVALID_ARGUMENT);
        assert(runtime.calls==calls);
        for (unsigned eye=0;eye<2;eye++) for (unsigned y=0;y<=8;y++) for (unsigned x=0;x<=8;x++) {
            float u=x/8.f,v=y/8.f;struct xrt_uv_triplet uv;
            assert(d->compute_distortion(d,eye,u,v,&uv)==XRT_SUCCESS);
            assert(fabsf(uv.r.x-(u*.8f+.1f))<1e-6f && fabsf(uv.r.y-(v*.6f+.2f))<1e-6f);
            assert(fabsf(uv.g.x-(u*.7f+.2f))<1e-6f && fabsf(uv.g.y-(v*.4f+.3f))<1e-6f);
            assert(fabsf(uv.b.x-(u*.5f+.3f))<1e-6f && fabsf(uv.b.y-(v*.9f+.05f))<1e-6f);samples++;
        }
        runtime.available=false;
        assert(d->update_inputs(d)==XRT_SUCCESS && !d->inputs[0].active && !d->inputs[0].timestamp);
        assert(d->get_tracked_pose(d,XRT_INPUT_GENERIC_HEAD_POSE,1,&pose)==XRT_SUCCESS && !pose.relation_flags);
        assert(pose.pose.orientation.w==1 && !pose.pose.position.x);
        calls=runtime.calls;d->destroy(d);d=NULL;assert(runtime.calls==calls);
    }
    printf("QUEST_MONADO_HMD_PASS cycles=200 optics_samples=%u borrowed_tracking=1 failed_pose_cleared=1\n",samples);
}
