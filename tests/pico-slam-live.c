#include "pico-slam-fixture.h"
#include "pico-slam-live.h"
#include "pico-camera-transport.h"
#include <errno.h>
#include <fcntl.h>

static unsigned io_fault, subscribed, unsubscribed, queued, camera_starts, camera_stops, live_cycles, live_faults;
static unsigned delivery, delivered_camera;
static bool nonblocking = true;
static uint64_t expected_first;
static int64_t completed_at, owner_now;
static unsigned clock_calls, fail_clock, clock_checks;
static bool regress_clock;
static int64_t live_clock(void *data)
{
    assert(data==&owner_now);
    if(++clock_calls==fail_clock)return regress_clock?owner_now-1:0;
    return owner_now;
}
struct pico_camera_rig { bool stopped, quiesced; };
struct pico_camera_transport {
    struct pico_camera_transport_config config;
    struct pico_camera_transport_ops ops;
    void *opaque;
    uint64_t pending[8], next, sequence;
    bool stopped, quiesced;
};
static struct pico_camera_transport *capture;
static struct pico_slam_live *retained[8];
static unsigned retained_count;
int fcntl(int fd, int op, ...)
{ assert((fd == 10 || fd == 12) && op == F_GETFL); return nonblocking ? O_NONBLOCK : 0; }
int pico_camera_event_subscribe(int fd, uint32_t id, bool enabled)
{
    assert((fd == 10 && id <= 1) || (fd == 12 && !id));
    if (enabled) { assert(!mock); ++subscribed; return io_fault == subscribed + 2 ? -EIO : 0; }
    assert(!capture); ++unsubscribed; return io_fault == 12 ? -EIO : 0;
}
int pico_camera_rig_prepare(const struct pico_camera_rig_config *c, struct pico_camera_rig **r)
{ assert(!mock && !*r && c->session.crm_fd == 10); *r=calloc(1,sizeof(**r)); assert(*r); return io_fault==1?-EIO:0; }
bool pico_camera_rig_view(const struct pico_camera_rig *r, struct pico_camera_session_view *v)
{
    assert(r && !mock); if(io_fault==2)return false;
    memset(v,0,sizeof(*v));v->session=100;v->capabilities.device_mmu=30;v->capabilities.cdm_mmu=31;
    for(unsigned i=0;i<2;++i){v->cameras[i].sensor=200+i;v->cameras[i].ife=300+i;v->cameras[i].link=400+i;}
    return true;
}
int pico_camera_rig_start(struct pico_camera_rig *r)
{ assert(r && !r->stopped && capture && queued >= 1 && created > destroyed); ++camera_starts; return io_fault==6?-EIO:0; }
int pico_camera_rig_stop(struct pico_camera_rig *r)
{ assert(r && !r->stopped && (!capture || capture->stopped)); r->stopped=true; ++camera_stops; return io_fault==11?-EIO:0; }
int pico_camera_rig_quiesced(struct pico_camera_rig *r)
{ assert(r && r->stopped && !capture && !r->quiesced); r->quiesced=true; return io_fault==13?-EIO:0; }
int pico_camera_rig_release(struct pico_camera_rig **r)
{ if(!*r)return 0; assert((*r)->quiesced); if(io_fault==14)return -EIO; free(*r); *r=NULL; return 0; }
int pico_camera_transport_create(const struct pico_camera_transport_config *c,
 const struct pico_camera_transport_ops *ops,void *opaque,struct pico_camera_transport **t)
{
    assert(!mock && !capture && !*t);
    if(io_fault==15)return -ENOMEM;
    for(unsigned i=0;i<2;i++) {
        const struct pico_camera_request_config *r=c->cameras+i;
        assert(r->crm_fd==10 && r->ife_fd==11 && r->sync_fd==12 && r->ion_fd==13 && r->sensor_fd==20+(int)i);
        assert(r->session==100 && r->link==400+(int)i && r->sensor==200+(int)i && r->ife==300+(int)i);
        assert(r->device_mmu==30 && r->command_mmu==31 && r->ion_heap==1 && r->command_mapping==PICO_CAMERA_MEMORY_SHARED);
        assert(r->width==3 && r->height==2 && r->stride==16 && r->camera_id==i && r->generation==55);
        assert(r->request_id==expected_first && r->synchronized);
    }
    *t=calloc(1,sizeof(**t));assert(*t);capture=*t;capture->config=*c;capture->ops=*ops;capture->opaque=opaque;
    capture->next=c->cameras[0].request_id;return 0;
}
int pico_camera_transport_queue(struct pico_camera_transport *t,const int64_t offsets[2],int64_t now)
{
    assert(t==capture && !t->stopped && offsets[0]==0 && offsets[1]==0 && now>0 && mock);
    if(io_fault==7)return -EIO;
    for(unsigned i=0;i<8;i++)if(!t->pending[i]){t->pending[i]=t->next++;++queued;return 0;}
    return -EAGAIN;
}
int pico_camera_transport_poll(struct pico_camera_transport *t,int64_t now)
{
    assert(t==capture && camera_starts && !t->stopped);
    if(io_fault==8){t->ops.invalidate(t->opaque,55);return -EIO;}
    if(!delivery)return 0;
    unsigned slot=8;
    for(unsigned i=0;i<8;i++)if(t->pending[i] && (slot==8 || t->pending[i]<t->pending[slot]))slot=i;
    assert(slot<8);
    uint8_t pixels[19]="abc";memcpy(pixels+16,"def",3);
    unsigned count=delivery==1?1:2;
    for(unsigned i=0;i<count;i++) {
        unsigned camera=delivery==3?1-i:delivered_camera++%2;
        struct pico_camera_capture_frame f={.buffer={.camera_id=camera,.request_id=t->pending[slot],
            .width=3,.height=2,.stride=16,.size=19,.pixels=pixels},.center_ns=completed_at ? completed_at : now,.frame_number=t->sequence};
        if(io_fault==9)f.buffer.request_id=UINT64_MAX;
        if(!t->ops.frame(t->opaque,55,&f,now)){t->ops.invalidate(t->opaque,55);return -EIO;}
    }
    if(count==2 || !(delivered_camera%2)){t->pending[slot]=0;++t->sequence;}
    return (int)count;
}
int pico_camera_transport_poll_clock(struct pico_camera_transport *t,
                                     int64_t (*clock)(void *), void *data)
{
    if(completed_at && delivery)owner_now=completed_at+1000;
    int64_t now=clock(data);
    if(now<=0){t->ops.invalidate(t->opaque,55);return -EIO;}
    return pico_camera_transport_poll(t,now);
}
void pico_camera_transport_stop(struct pico_camera_transport *t)
{ assert(t==capture); if(!t->stopped){t->stopped=true;t->ops.invalidate(t->opaque,55);} }
int pico_camera_transport_quiesced(struct pico_camera_transport *t)
{ assert(t==capture && t->stopped && !t->quiesced); t->quiesced=true; return io_fault==10?-EIO:0; }
int pico_camera_transport_destroy(struct pico_camera_transport **t)
{ assert(*t==capture && capture->quiesced); free(*t);*t=NULL;capture=NULL;return 0; }
static struct pico_slam_live_config config(void)
{
    subscribed=unsubscribed=queued=camera_starts=camera_stops=delivered_camera=delivery=0;
    expected_first=10;
    struct pico_slam_live_config c={.tracking=fixture_config(),.sync_fd=12,.ion_fd=13,.ion_heap=1,.stride={16,16},
        .queue_depth=2,.first_request_id=expected_first,.frame_period_ns=2000000,.capture_timeout_ns=100000000};
    c.cameras.session.crm_fd=10;c.cameras.session.ife_fd=11;c.cameras.command_mapping=PICO_CAMERA_MEMORY_SHARED;
    for(unsigned i=0;i<2;i++) {
        c.cameras.session.cameras[i].sensor_fd=20+i;
        c.cameras.session.cameras[i].input.width=3;c.cameras.session.cameras[i].input.height=2;
    }
    return c;
}
static bool has_pose(struct pico_slam_live *s,int64_t now)
{
    struct xrt_space_relation r;int64_t capture_ns;
    return pico_slam_tracking_get(pico_slam_live_tracking(s),now,now,&r,&capture_ns);
}
static void finish(struct pico_slam_live **s)
{
    assert(pico_slam_live_destroy(s)==-EBUSY);
    assert(!pico_slam_live_stop(*s));assert(!pico_slam_live_stop(*s));
    assert(!pico_slam_live_quiesced(*s));assert(!pico_slam_live_quiesced(*s));
    assert(!pico_slam_live_destroy(s) && !*s && !mock && !capture);
    assert(subscribed==unsubscribed && camera_stops==1);
}
int main(void)
{
    owner=pthread_self();int64_t now=1000000000,offsets[2]={0,0};size_t poses;
    struct pico_slam_live *s=NULL;struct pico_slam_live_config c=config(),bad;
    for(unsigned n=0;n<7;n++) {
        bad=c;
        if(n==0)bad.queue_depth=9;
        if(n==1)bad.first_request_id=0;
        if(n==2)bad.tracking.input.width[1]=4;
        if(n==3)bad.capture_timeout_ns=1;
        if(n==4)bad.stride[0]=17;
        if(n==5)bad.sync_fd=10;
        if(n==6)nonblocking=false;
        assert(pico_slam_live_prepare(&bad,&s)<0 && !s && !mock && !capture);nonblocking=true;
    }
    for(unsigned cycle=0;cycle<200;cycle++) {
        c=config();c.queue_depth=cycle%8+1;
        assert(!pico_slam_live_prepare(&c,&s) && mock && subscribed==3 && !queued && !camera_starts);
        assert(pico_slam_live_poll(s,offsets,now,&poses)==0 && !poses && !queued);
        paired(now-1000,now,1);
        assert(pico_slam_live_poll(s,offsets,now,&poses)==1 && !poses && queued==c.queue_depth && camera_starts==1);
        for(unsigned set=0;set<4;set++) {
            int64_t at=now+(set+1)*2000000;
            paired(at-1000,at,1);delivery=set%2?3:2;
            unsigned before=queued;
            assert(pico_slam_live_poll(s,offsets,at,&poses)==1 && !poses && queued==before+1);
            paired(at+1000,at+10000,1);delivery=0;
            assert(pico_slam_live_poll(s,offsets,at+10000,&poses)==1 && poses==1);
            assert(has_pose(s,at+10000)==(set>0));
        }
        finish(&s);++live_cycles;
    }
    c=config();assert(!pico_slam_live_prepare(&c,&s));paired(now-1000,now,1);
    assert(pico_slam_live_poll(s,offsets,now,&poses)==1);
    completed_at=now+1000;delivery=2;owner_now=now;
    assert(pico_slam_live_poll_clock(s,offsets,live_clock,&owner_now,&poses)==1 && !poses);
    completed_at=0;delivery=0;owner_now=now+4000;
    paired(now+3000,owner_now,1);
    assert(pico_slam_live_poll_clock(s,offsets,live_clock,&owner_now,&poses)==1 && poses==1);
    finish(&s);++clock_checks;
    for(unsigned n=0;n<5;n++) {
        c=config();assert(!pico_slam_live_prepare(&c,&s));paired(now-1000,now,1);
        assert(pico_slam_live_poll(s,offsets,now,&poses)==1);
        owner_now=now+2000;clock_calls=0;fail_clock=n<3?n+1:n-1;regress_clock=n>=3;
        assert(pico_slam_live_poll_clock(s,offsets,live_clock,&owner_now,&poses)<0 && !poses);
        fail_clock=0;regress_clock=false;finish(&s);++clock_checks;
    }
    c=config();assert(!pico_slam_live_prepare(&c,&s));
    assert(pico_slam_live_poll_clock(s,offsets,NULL,NULL,&poses)==-EINVAL && !poses);
    finish(&s);++clock_checks;
    printf("PICO_SLAM_LIVE_CLOCK_PASS cases=%u arrival_after_poll_start=1\n",clock_checks);
    c=config();bad=c;bad.tracking.generation=0;
    assert(pico_slam_live_prepare(&bad,&s)<0 && s && !mock && !capture);
    assert(!pico_slam_live_stop(s) && !pico_slam_live_quiesced(s) && !pico_slam_live_destroy(&s));
    assert(!camera_stops && !subscribed);++live_faults;
    c=config();assert(!pico_slam_live_prepare(&c,&s));paired(now-1000,now,1);
    assert(pico_slam_live_poll(s,offsets,now,&poses)==1);
    unsigned before=queued;delivery=1;
    assert(pico_slam_live_poll(s,offsets,now,&poses)==1 && queued==before);
    assert(pico_slam_live_poll(s,offsets,now+100,&poses)==1 && queued==before+1);
    paired(now+1000,now+10000,1);delivery=0;
    assert(pico_slam_live_poll(s,offsets,now+10000,&poses)==1 && poses==1);
    assert(pico_slam_live_poll(s,offsets,c.tracking.clock_expires_ns,&poses)<0 && !poses);
    finish(&s);++live_faults;
    for(io_fault=1;io_fault<=15;io_fault++) {
        if(io_fault>=10 && io_fault<=14)continue;
        c=config();int result=pico_slam_live_prepare(&c,&s);assert(s);
        if(io_fault<=5 || io_fault==15) assert(result<0 && !mock);
        else {
            assert(!result);paired(now-1000,now,1);delivery=2;
            assert(pico_slam_live_poll(s,offsets,now,&poses)<0);
            assert(!has_pose(s,now));
        }
        finish(&s);++live_faults;
    }
    io_fault=0;c=config();fail_create=true;
    assert(pico_slam_live_prepare(&c,&s)<0 && s && !mock);fail_create=false;finish(&s);++live_faults;
    c=config();expected_first=c.first_request_id=INT64_MAX;c.queue_depth=2;
    assert(!pico_slam_live_prepare(&c,&s));paired(now-1000,now,1);
    assert(pico_slam_live_poll(s,offsets,now,&poses)==-EOVERFLOW);finish(&s);++live_faults;
    c=config();assert(!pico_slam_live_prepare(&c,&s));paired(now-1000,now,1);
    assert(pico_slam_live_poll(s,offsets,now,&poses)==1);send(2,NULL,now);
    assert(pico_slam_live_poll(s,offsets,now,&poses)<0 && !poses);finish(&s);++live_faults;
    assert(created==destroyed && sensor_created==sensor_destroyed && pose_created==pose_destroyed);
    for(io_fault=10;io_fault<=14;io_fault++) {
        c=config();fail_create=true;assert(pico_slam_live_prepare(&c,&s)<0 && !mock);fail_create=false;
        int stop_result=pico_slam_live_stop(s);assert(io_fault==11?stop_result<0:stop_result==0);
        int q=pico_slam_live_quiesced(s);assert(io_fault==14?q==0:q<0);
        assert(pico_slam_live_quiesced(s)==q);
        assert(pico_slam_live_destroy(&s)<0 && s);
        assert(pico_slam_live_destroy(&s)<0 && s);
        retained[retained_count++]=s;s=NULL;
        /* The failed transport remains reachable from the retained owner. */
        capture=NULL;++live_faults;
    }
    printf("PICO_SLAM_LIVE_PASS cycles=%u faults=%u retained=%u poses=%u\n",live_cycles,live_faults,retained_count,pose_destroyed);
}
