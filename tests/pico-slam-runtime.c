#include "pico-slam-fixture.h"

static bool tracked(struct pico_slam_runtime *s, int64_t now)
{
    struct xrt_space_relation relation; int64_t capture;
    return pico_slam_tracking_get(pico_slam_runtime_tracking(s), now, now, &relation, &capture);
}
static bool feed(struct pico_slam_runtime *s, uint64_t generation, int64_t now, uint64_t seq)
{
    size_t poses;
    paired(now-1000, now, 1);
    if (pico_slam_runtime_poll(s, now, &poses) != 1) return false;
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t pixels[19] = "abc";
        memcpy(pixels+16, "def", 3);
        struct pico_camera_capture_frame frame = {
            .buffer = {.camera_id=i, .width=3, .height=2, .stride=16, .size=19, .pixels=pixels},
            .center_ns=now, .frame_number=seq};
        if (!pico_slam_runtime_captured(s, generation, &frame, now)) return false;
        memset(pixels, 0xa5, sizeof(pixels));
    }
    paired(now+1000, now+10000, 1);
    return pico_slam_runtime_poll(s, now+10000, &poses) == 1 && poses == 1;
}
int main(void)
{
    owner = pthread_self();
    struct pico_slam_runtime_config c = fixture_config();
    struct pico_slam_runtime *s = NULL;
    struct pico_slam_runtime_config bad = c; bad.sensors.bias.rigid_body[1] = 1;
    assert(!pico_slam_runtime_create(&bad, &s));
    bad=c; bad.generation=0; assert(!pico_slam_runtime_create(&bad, &s));
    bad=c; bad.sensors.bias.report=(void*)1; assert(!pico_slam_runtime_create(&bad, &s));
    fail_create=true; assert(!pico_slam_runtime_create(&c, &s)); fail_create=false;
    int64_t base = 1000000000;
    size_t poses;
    for (unsigned cycle = 0; cycle < 200; ++cycle) {
        assert(pico_slam_runtime_create(&c, &s));
        assert(!pico_slam_runtime_create(&c, &s));
        send(4, NULL, base);
        assert(pico_slam_runtime_poll(s, base, &poses) == 0 && poses == 0);
        assert(!pico_slam_runtime_captured(s, c.generation, NULL, base));
        unsigned before = created;
        assert(feed(s, c.generation, base+10000, 0));
        assert(created == before+1 && !tracked(s, base+20000));
        assert(feed(s, c.generation, base+2010000, 1));
        assert(tracked(s, base+2020000));
        assert(!pico_slam_runtime_captured(s, c.generation-1, NULL, base+2020000));
        assert(tracked(s, base+2020000));
        if (cycle % 2) {
            send(2, NULL, base+2020000);
            assert(!tracked(s, base+2020000));
            assert(pico_slam_runtime_poll(s, base+2020000, &poses) < 0);
        } else {
            pico_slam_runtime_stop(s);
            pico_slam_runtime_stop(s);
            assert(!tracked(s, base+2020000));
        }
        assert(pico_slam_runtime_destroy(&s) && !s);
    }
    for (fault=1; fault<=8; ++fault) {
        assert(pico_slam_runtime_create(&c, &s));
        assert(!feed(s, c.generation, base+10000, 0));
        assert(!tracked(s, base+20000));
        assert(pico_slam_runtime_destroy(&s));
    }
    fault=0;
    for (unsigned mode=0; mode<7; ++mode) {
        assert(pico_slam_runtime_create(&c, &s));
        assert(feed(s, c.generation, base+10000, 0));
        assert(feed(s, c.generation, base+2010000, 1));
        assert(tracked(s, base+2020000));
        if (mode==0) paired(base+3000000, base+3000001, 2);
        if (mode==1) paired(base+2011000, base+3000001, 1);
        if (mode==2) {
            struct ssc_bias_sample bad_sample={.raw={.generation=1},.accuracy=0};
            send(1,&bad_sample,base+3000001);
        }
        if (mode==3) assert(!pico_slam_runtime_captured(s,c.generation,NULL,base+3000001));
        if (mode==4) assert(pico_slam_runtime_poll(s,base+20000000,&poses)<0);
        if (mode==5) assert(pico_slam_runtime_poll(s,c.clock_expires_ns,&poses)<0);
        if (mode==6) assert(pico_slam_runtime_poll(s,base+10000,&poses)<0);
        assert(!tracked(s, base+3000001));
        assert(pico_slam_runtime_poll(s,base+3000001,&poses)<0 && poses==0);
        assert(pico_slam_runtime_destroy(&s));
    }
    assert(pico_slam_runtime_destroy(&s));
    unsigned before_sensors=sensor_created,before_trackers=created;
    assert(pico_slam_runtime_prepare(&c,&s) && !mock && sensor_created==before_sensors);
    assert(!pico_slam_runtime_prepare(&c,&s));
    assert(pico_slam_runtime_poll(s,base,&poses)==0 && !poses && created==before_trackers);
    assert(pico_slam_runtime_destroy(&s) && !mock && sensor_created==before_sensors);
    assert(pico_slam_runtime_prepare(&c,&s));
    pico_slam_runtime_stop(s);
    assert(!pico_slam_runtime_start(s) && !mock);
    assert(pico_slam_runtime_destroy(&s));
    assert(pico_slam_runtime_prepare(&c,&s));
    fail_create=true;assert(!pico_slam_runtime_start(s) && s && !mock);fail_create=false;
    assert(!pico_slam_runtime_start(s) && !mock);
    assert(pico_slam_runtime_poll(s,base,&poses)<0 && !poses);
    assert(pico_slam_runtime_destroy(&s));
    assert(pico_slam_runtime_prepare(&c,&s) && sensor_created==before_sensors);
    assert(pico_slam_runtime_poll(s,base,&poses)==0 && !poses);
    assert(pico_slam_runtime_start(s) && sensor_created==before_sensors+1);
    assert(!pico_slam_runtime_start(s) && sensor_created==before_sensors+1);
    assert(feed(s,c.generation,base+10000,0));
    assert(pico_slam_runtime_destroy(&s));
    assert(!pico_slam_runtime_start(NULL));
    assert(created==destroyed && created==stopped && pose_created==pose_destroyed);
    assert(sensor_created==sensor_destroyed);
    printf("PICO_SLAM_RUNTIME_PASS cycles=200 backend_faults=8 stream_faults=7 deferred=4 joined=%u trackers=%u poses=%u\n",sensor_destroyed,destroyed,pose_destroyed);
}
