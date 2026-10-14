#include "pico-slam-imu.h"
#include <assert.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const struct pico_slam_imu_config config = {
    .sensor_to_imu = {0,-1,0,1,0,0,0,0,1},
    .acceleration_scale = 2, .angular_velocity_scale = 0.5,
    .max_age_ns = 1000000000, .max_gap_ns = 100000000, .minimum_accuracy = 2,
};
static struct pico_slam_imu *q;
static uint64_t generation;
static unsigned faults;
static const int64_t base = 1000000000;

static void begin(void)
{
    assert(pico_slam_imu_begin(q, ++generation, base));
}
static struct ssc_bias_sample sample(unsigned sensor, int64_t timestamp)
{
    struct ssc_bias_sample s = {0};
    s.raw.sensor = sensor;
    s.raw.time = (struct ssc_timestamp){timestamp, 3};
    s.raw.generation = generation;
    s.raw.ticks = (uint64_t)timestamp;
    s.accuracy = 3;
    s.corrected[0] = 1; s.corrected[1] = 2; s.corrected[2] = 3;
    return s;
}
static void push_pair(int64_t time, int64_t now)
{
    struct ssc_bias_sample a = sample(0, time), g = sample(1, time);
    g.raw.time.uncertainty_ns = 5;
    assert(pico_slam_imu_push(q, &g, now));
    assert(pico_slam_imu_push(q, &a, now));
}
static int pop(int64_t now, struct xrt_imu_sample *v, int64_t *u)
{
    return pico_slam_imu_pop(q, generation, now, v, u);
}
static void unchanged(int64_t now, int expected)
{
    struct xrt_imu_sample v, before;
    memset(&v, 0x5a, sizeof(v)); before = v;
    int64_t u = 55;
    assert(pop(now, &v, &u) == expected);
    assert(!memcmp(&v, &before, sizeof(v)) && u == 55);
}
static void rejected(struct ssc_bias_sample s, int64_t now)
{
    assert(!pico_slam_imu_push(q, &s, now));
    unchanged(now, -ECANCELED);
    ++faults;
}

static atomic_uint consumed;
static void *producer(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 10000; ++i) {
        while (i >= atomic_load(&consumed) + 16) sched_yield();
        push_pair(base + 100*i, base + 10000000);
    }
    return NULL;
}

int main(void)
{
    struct pico_slam_imu_config bad = config;
    bad.sensor_to_imu[0] = 1;
    assert(!pico_slam_imu_create(&bad, &q));
    bad = config; bad.sensor_to_imu[8] = -1;
    assert(!pico_slam_imu_create(&bad, &q));
    bad = config; bad.acceleration_scale = NAN;
    assert(!pico_slam_imu_create(&bad, &q));
    bad = config; bad.angular_velocity_scale = 0;
    assert(!pico_slam_imu_create(&bad, &q));
    bad = config; bad.max_gap_ns = bad.max_age_ns + 1;
    assert(!pico_slam_imu_create(&bad, &q));
    bad = config; bad.minimum_accuracy = 0;
    assert(!pico_slam_imu_create(&bad, &q));
    assert(pico_slam_imu_create(&config, &q));
    assert(!pico_slam_imu_create(&config, &q));
    begin();
    unchanged(base, 0);
    push_pair(base, base+5);
    struct xrt_imu_sample v;
    int64_t u;
    assert(pop(base+5, &v, &u) == 1 && u == 5);
    assert(v.timestamp_ns == base && v.accel_m_s2.x == -4 && v.accel_m_s2.y == 2 && v.accel_m_s2.z == 6);
    assert(v.gyro_rad_secs.x == -1 && v.gyro_rad_secs.y == 0.5 && v.gyro_rad_secs.z == 1.5);
    unchanged(base+5, 0);
    unchanged(base+config.max_age_ns-4, -ETIMEDOUT);
    ++faults;

    begin();
    push_pair(base+100, base+105);
    unchanged(base+50, 0);
    unchanged(base+104, 0);
    assert(pop(base+105, &v, &u) == 1);
    struct ssc_bias_sample stale = sample(0, base+200);
    --stale.raw.generation;
    assert(!pico_slam_imu_push(q, &stale, base+205));
    pico_slam_imu_stop(q, generation-1);
    assert(pico_slam_imu_pop(q, generation-1, base, &v, &u) == -ESTALE);
    assert(!pico_slam_imu_begin(q, generation, base));
    unchanged(base+106, 0);
    // Consumer time must not constrain the sensor worker's earlier clock read.
    push_pair(base+200, base+205);
    assert(pop(base+1000, &v, &u) == 1);
    push_pair(base+300, base+305);
    assert(pop(base+1000, &v, &u) == 1 && v.timestamp_ns == base+300);

    assert(pico_slam_imu_begin(q, ++generation, base+100));
    unchanged(base+50, 0);
    push_pair(base+100, base+105);
    assert(pop(base+105, &v, &u) == 1);

    for (unsigned i = 0; i < 12; ++i) {
        begin();
        struct ssc_bias_sample s = sample(0, base);
        int64_t now = base+5;
        switch (i) {
        case 0: s.raw.sensor = 2; break;
        case 1: s.accuracy = 0; break;
        case 2: s.accuracy = 4; break;
        case 3: s.corrected[1] = NAN; break;
        case 4: s.corrected[2] = FLT_MAX; break;
        case 5: s.raw.time.uncertainty_ns = -1; break;
        case 6: s.raw.time.timestamp_ns = 0; break;
        case 7: s.raw.time.timestamp_ns = INT64_MAX; break;
        case 8: now = base+config.max_age_ns; break;
        case 9: s.raw.ticks = 0; break;
        case 10: now = base-1; break;
        case 11: s.raw.time.uncertainty_ns = INT64_MAX; break;
        }
        rejected(s, now);
    }
    begin();
    push_pair(base, base+5);
    rejected(sample(0, base), base+5);
    begin();
    push_pair(base, base+5);
    rejected(sample(1, base+config.max_gap_ns+1), base+config.max_gap_ns+6);
    begin();
    struct ssc_bias_sample a = sample(0, base), g = sample(1, base);
    assert(pico_slam_imu_push(q, &a, base+5));
    ++g.raw.ticks;
    rejected(g, base+5);
    begin();
    a = sample(0, base); g = sample(1, base+1); g.raw.ticks = a.raw.ticks;
    assert(pico_slam_imu_push(q, &a, base+6));
    rejected(g, base+6);
    begin();
    for (unsigned i = 0; i < 32; ++i) {
        a = sample(0, base+i*100);
        assert(pico_slam_imu_push(q, &a, base+10000));
    }
    rejected(sample(0, base+3200), base+10000);
    begin();
    for (unsigned i = 0; i < 128; ++i) push_pair(base+i*100, base+100000);
    a = sample(0, base+12800); g = sample(1, base+12800);
    assert(pico_slam_imu_push(q, &a, base+100000));
    rejected(g, base+100000);
    begin();
    unchanged(base+config.max_age_ns+1, -ETIMEDOUT); ++faults;
    begin();
    push_pair(base, base+5);
    unchanged(base+config.max_age_ns-4, -ETIMEDOUT); ++faults;
    begin();
    unchanged(base+100, 0);
    unchanged(base+99, -ETIMEDOUT); ++faults;

    for (unsigned cycle = 0; cycle < 200; ++cycle) {
        begin();
        a = sample(0, base); g = sample(1, base+100);
        assert(pico_slam_imu_push(q, &a, base+105));
        assert(pico_slam_imu_push(q, &g, base+105));
        unchanged(base+105, 0);
        a = sample(0, base+100);
        assert(pico_slam_imu_push(q, &a, base+105));
        assert(pop(base+105, &v, &u) == 1 && v.timestamp_ns == base+100);
        for (unsigned i = 2; i < 300; ++i) {
            push_pair(base+i*100, base+i*100+5);
            assert(pop(base+i*100+5, &v, &u) == 1 && v.timestamp_ns == base+i*100);
        }
        pico_slam_imu_stop(q, generation);
        unchanged(base+30000, -ECANCELED);
    }
    begin();
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, producer, NULL));
    for (unsigned i = 0; i < 10000;) {
        int status = pop(base+20000000, &v, &u);
        assert(status >= 0);
        if (status) {
            assert(v.timestamp_ns == base+100*i && u == 5);
            atomic_store(&consumed, ++i);
        } else sched_yield();
    }
    assert(!pthread_join(worker, NULL));
    pico_slam_imu_destroy(&q);
    assert(!q);
    pico_slam_imu_destroy(&q);
    printf("PICO_SLAM_IMU_PASS cycles=200 pairs=59800 threaded_pairs=10000 faults=%u\n", faults);
    return 0;
}
