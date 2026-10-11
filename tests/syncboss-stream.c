#include "syncboss-imu.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;

static struct syncboss_imu_direct_record record(uint32_t counter, uint64_t tick)
{
    struct syncboss_imu_direct_record result = {0};
    result.counter = counter;
    result.sample.timestamp_ticks = tick;
    result.timestamp_ns = (int64_t)tick * 1000;
    result.delivery_ns = result.timestamp_ns + 500000;
    result.sample.temperature_c = 25;
    result.sample.acceleration_m_s2[2] = 9.80665f;
    return result;
}

static void rejected(struct syncboss_imu_stream *stream,
                     struct syncboss_imu_direct_record *input, int64_t now,
                     int64_t limit, enum syncboss_imu_stream_status status)
{
    struct syncboss_imu_stream before;
    memcpy(&before, stream, sizeof(before));
    uint32_t dropped = 0xfeed;
    assert(syncboss_imu_stream_accept(stream, input, now, limit, &dropped) == status);
    assert(memcmp(&before, stream, sizeof(before)) == 0 && dropped == 0xfeed);
    ++checks;
}

int main(void)
{
    struct syncboss_imu_stream stream = {0};
    uint32_t dropped;
    uint32_t counter = UINT32_MAX - 100;
    uint64_t tick = 1000000;
    for (unsigned i = 0; i < 10000; ++i) {
        struct syncboss_imu_direct_record sample = record(counter, tick);
        assert(syncboss_imu_stream_accept(&stream, &sample, sample.delivery_ns + 100000,
                                         1000000, &dropped) == SYNCBOSS_IMU_ACCEPTED);
        assert(dropped == (i && i % 13 == 0 ? 1u : 0u));
        rejected(&stream, &sample, sample.delivery_ns, 1000000, SYNCBOSS_IMU_OUT_OF_ORDER);
        counter = counter == UINT32_MAX ? 1 : counter + 1;
        tick += 1250;
        if ((i + 1) % 13 == 0) {
            counter = counter == UINT32_MAX ? 1 : counter + 1;
            tick += 1250;
        }
        ++checks;
    }
    struct syncboss_imu_direct_record next = record(counter, tick), bad;
    rejected(&stream, &next, next.delivery_ns + 500001, 1000000, SYNCBOSS_IMU_STALE);
    rejected(&stream, &next, next.delivery_ns, 0, SYNCBOSS_IMU_INVALID);
    rejected(&stream, &next, -1, 1000000, SYNCBOSS_IMU_INVALID);
    rejected(&stream, &next, next.delivery_ns - 1, 1000000, SYNCBOSS_IMU_INVALID);
    bad = next; bad.timestamp_ns = -1;
    rejected(&stream, &bad, next.delivery_ns, 1000000, SYNCBOSS_IMU_INVALID);
    bad = next; bad.delivery_ns = next.timestamp_ns - 1;
    rejected(&stream, &bad, next.delivery_ns, 1000000, SYNCBOSS_IMU_INVALID);
    bad = next; bad.counter = 0;
    rejected(&stream, &bad, next.delivery_ns, 1000000, SYNCBOSS_IMU_INVALID);
    bad = next; bad.counter = stream.counter - 1;
    rejected(&stream, &bad, next.delivery_ns, 1000000, SYNCBOSS_IMU_OUT_OF_ORDER);
    bad = next; bad.sample.timestamp_ticks = stream.timestamp_ticks;
    rejected(&stream, &bad, next.delivery_ns, 1000000, SYNCBOSS_IMU_OUT_OF_ORDER);
    bad = next; bad.timestamp_ns = stream.timestamp_ns;
    rejected(&stream, &bad, next.delivery_ns, INT64_MAX, SYNCBOSS_IMU_OUT_OF_ORDER);
    bad = next; bad.sample.timestamp_ticks = 0;
    rejected(&stream, &bad, next.delivery_ns, 1000000, SYNCBOSS_IMU_OUT_OF_ORDER);
    bad = next; bad.sample.device_id = 1;
    rejected(&stream, &bad, next.delivery_ns, 1000000, SYNCBOSS_IMU_INVALID);
    bad = next; bad.sample.temperature_c = NAN;
    rejected(&stream, &bad, next.delivery_ns, 1000000, SYNCBOSS_IMU_INVALID);
    for (unsigned axis = 0; axis < 3; ++axis) {
        bad = next; bad.sample.acceleration_m_s2[axis] = INFINITY;
        rejected(&stream, &bad, next.delivery_ns, 1000000, SYNCBOSS_IMU_INVALID);
        bad = next; bad.sample.angular_velocity_rad_s[axis] = NAN;
        rejected(&stream, &bad, next.delivery_ns, 1000000, SYNCBOSS_IMU_INVALID);
    }
    assert(syncboss_imu_stream_accept(&stream, &next, next.delivery_ns + 500000,
                                     1000000, &dropped) == SYNCBOSS_IMU_ACCEPTED);
    assert(dropped == 0);
    memset(&stream, 0, sizeof(stream));
    next = record(1, 1);
    assert(syncboss_imu_stream_accept(&stream, &next, INT64_MAX, INT64_MAX,
                                     &dropped) == SYNCBOSS_IMU_ACCEPTED);
    assert(!dropped);
    assert(syncboss_imu_stream_accept(NULL, &next, 0, 1, &dropped) == SYNCBOSS_IMU_INVALID);
    assert(syncboss_imu_stream_accept(&stream, NULL, 0, 1, &dropped) == SYNCBOSS_IMU_INVALID);
    assert(syncboss_imu_stream_accept(&stream, &next, 0, 1, NULL) == SYNCBOSS_IMU_INVALID);
    printf("%u stream freshness and ordering checks passed\n", checks);
    return 0;
}
