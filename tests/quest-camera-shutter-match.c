#include "quest-camera-shutter.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned cases;
static void rejected(const struct quest_camera_metadata *m, const struct quest_camera_match_window *w,
                     const struct quest_camera_shutter_candidate *c, size_t n, int expected)
{
    struct quest_camera_shutter out, before; memset(&out,0xa5,sizeof(out)); before=out;
    assert(quest_camera_shutter_select(m,w,c,n,&out)==expected && !memcmp(&out,&before,sizeof(out)));cases++;
}
int main(void)
{
    struct quest_camera_metadata m={.bits_per_pixel=10};
    struct quest_camera_match_window w={.generation=17,.minimum_timestamp_us=1000,.maximum_timestamp_us=2000,
        .now_ns=5000000,.max_age_ns=1000000,.camera_bit=1};
    struct quest_camera_shutter_candidate c[16]={0};
    c[0]=(struct quest_camera_shutter_candidate){.shutter={.timestamp_us=1500,.camera_mask=1},.generation=17,.received_ns=4500000};
    const unsigned types[]={6,21,22};
    for(unsigned t=0;t<3;t++)for(unsigned policy=0;policy<2;policy++)for(unsigned tag=0;tag<256;tag++)for(unsigned flags=0;flags<4;flags++) {
        c[0].shutter.type=types[t];w.require_auxiliary_tag=policy;
        m.tag=tag;m.auxiliary_tag=255-tag;c[0].shutter.tag=tag+(flags&1);c[0].shutter.auxiliary_tag=m.auxiliary_tag+((flags>>1)&1);
        bool expected=(types[t]==21||!(flags&1))&&(!(policy||types[t]==22)||!(flags&2));
        if(!expected)rejected(&m,&w,c,1,0);
        else {struct quest_camera_shutter out;assert(quest_camera_shutter_select(&m,&w,c,1,&out)==1&&!memcmp(&out,&c[0].shutter,sizeof(out)));cases++;}
    }
    c[0].shutter.type=21;c[0].shutter.tag=0;c[0].shutter.auxiliary_tag=m.auxiliary_tag;w.require_auxiliary_tag=false;
    rejected(&m,&w,NULL,0,0);
    c[1]=c[0];rejected(&m,&w,c,2,-EEXIST);
    c[1].shutter.timestamp_us++;rejected(&m,&w,c,2,-EEXIST);
    c[1].generation=16;struct quest_camera_shutter out;assert(quest_camera_shutter_select(&m,&w,c,2,&out)==1);cases++;
    c[0].generation=18;rejected(&m,&w,c,2,0);c[0].generation=17;
    c[0].received_ns=3999999;rejected(&m,&w,c,1,0);c[0].received_ns=4000000;assert(quest_camera_shutter_select(&m,&w,c,1,&out)==1);cases++;
    c[0].received_ns=5000001;rejected(&m,&w,c,1,-EPROTO);c[0].received_ns=0;rejected(&m,&w,c,1,-EPROTO);c[0].received_ns=4500000;
    const uint64_t times[]={999,1000,2000,2001};
    for(unsigned i=0;i<4;i++){c[0].shutter.timestamp_us=times[i];if(i==0||i==3)rejected(&m,&w,c,1,0);else{assert(quest_camera_shutter_select(&m,&w,c,1,&out)==1);cases++;}}
    c[0].shutter.timestamp_us=1500;w.after_timestamp_us=1500;rejected(&m,&w,c,1,0);w.after_timestamp_us=1499;assert(quest_camera_shutter_select(&m,&w,c,1,&out)==1);cases++;w.after_timestamp_us=0;
    for(unsigned i=1;i<16;i++){c[i]=c[0];c[i].shutter.timestamp_us=900;}assert(quest_camera_shutter_select(&m,&w,c,16,&out)==1);cases++;
    c[15].shutter.timestamp_us=1500;rejected(&m,&w,c,16,-EEXIST);c[15].shutter.type=99;rejected(&m,&w,c,16,-EPROTO);
    c[15].generation=16;assert(quest_camera_shutter_select(&m,&w,c,16,&out)==1);cases++;
    c[0].shutter.camera_mask=2;rejected(&m,&w,c,1,0);c[0].shutter.camera_mask=0;rejected(&m,&w,c,1,-EPROTO);c[0].shutter.camera_mask=1;
    c[0].shutter.timestamp_us=0;rejected(&m,&w,c,1,-EPROTO);c[0].shutter.timestamp_us=1500;
    rejected(NULL,&w,c,1,-EINVAL);rejected(&m,NULL,c,1,-EINVAL);rejected(&m,&w,NULL,1,-EINVAL);rejected(&m,&w,c,17,-EINVAL);rejected(&m,&w,c,SIZE_MAX,-EINVAL);
    assert(quest_camera_shutter_select(&m,&w,c,1,NULL)==-EINVAL);cases++;
    m.bits_per_pixel=16;rejected(&m,&w,c,1,-EINVAL);m.bits_per_pixel=8;
    struct quest_camera_match_window valid=w;
#define INVALID(field,value) do{w.field=value;rejected(&m,&w,c,1,-EINVAL);w=valid;}while(0)
    INVALID(generation,0);INVALID(camera_bit,0);INVALID(camera_bit,3);INVALID(now_ns,0);INVALID(now_ns,-1);
    INVALID(max_age_ns,0);INVALID(max_age_ns,1000000001);INVALID(minimum_timestamp_us,0);INVALID(minimum_timestamp_us,2001);
    INVALID(maximum_timestamp_us,999);INVALID(after_timestamp_us,2000);INVALID(after_timestamp_us,UINT64_MAX);
    w.minimum_timestamp_us=w.maximum_timestamp_us=UINT64_MAX;w.after_timestamp_us=UINT64_MAX-1;
    c[0].shutter.timestamp_us=UINT64_MAX;assert(quest_camera_shutter_select(&m,&w,c,1,&out)==1);cases++;
    w.now_ns=INT64_MAX;c[0].received_ns=INT64_MAX;assert(quest_camera_shutter_select(&m,&w,c,1,&out)==1);cases++;
    assert(quest_camera_shutter_select(&m,&w,c,1,&c[0].shutter)==1);cases++;
    printf("QUEST_CAMERA_SHUTTER_MATCH_PASS cases=%u hardware=0\n",cases);
}
