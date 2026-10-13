#include "pico-head-tracking.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    FILE *input = fopen(argv[1], "r");
    if (!input) return 2;
    const struct pico_head_tracking_config config = {
        .sensor_to_head = {1,0,0, 0,1,0, 0,0,1},
        .acceleration_scale = 1, .angular_velocity_scale = 1,
        .max_age_ns = 100000000, .max_gap_ns = 20000000, .minimum_accuracy = 1,
    };
    struct pico_head_tracking *tracking = NULL;
    assert(pico_head_tracking_create(&config, &tracking));
    assert(pico_head_tracking_begin(tracking, 1));
    unsigned samples = 0, orientations = 0;
    int64_t now = 0, expected;
    struct xrt_space_relation relation;
    for (;;) {
        unsigned sensor;
        struct ssc_imu_event event = {.generation = 1, .vector.accuracy = 3};
        int count = fscanf(input, "%u %" SCNu64 " %" SCNd64 " %" SCNd64 " %f %f %f %" SCNd64,
                           &sensor, &event.ticks, &event.time.timestamp_ns, &event.time.uncertainty_ns,
                           &event.vector.values[0], &event.vector.values[1], &event.vector.values[2], &expected);
        if (count == EOF) break;
        assert(count == 8 && sensor < 2 && ++samples <= 8192);
        event.sensor = sensor;
        assert(event.time.timestamp_ns > 0 && event.time.timestamp_ns < INT64_MAX - 1000000000);
        assert(event.time.uncertainty_ns >= 0 && event.time.uncertainty_ns < 1000000);
        int64_t available = event.time.timestamp_ns + event.time.uncertainty_ns;
        if (available > now) now = available;
        assert(pico_head_tracking_push(tracking, &event, now));
        int64_t capture;
        bool valid = pico_head_tracking_get(tracking, now, &relation, &capture);
        assert(valid == (expected != 0) && capture == expected);
        if (valid) {
            assert(relation.relation_flags == (XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT));
            double norm = (double)relation.pose.orientation.x * relation.pose.orientation.x +
                          (double)relation.pose.orientation.y * relation.pose.orientation.y +
                          (double)relation.pose.orientation.z * relation.pose.orientation.z +
                          (double)relation.pose.orientation.w * relation.pose.orientation.w;
            assert(isfinite(norm) && fabs(norm - 1) < 0.001);
            orientations++;
        } else assert(!relation.relation_flags);
    }
    assert(!ferror(input) && !fclose(input) && samples > 20 && orientations > 10);
    int64_t capture;
    assert(!pico_head_tracking_get(tracking, now + config.max_age_ns + 1, &relation, &capture));
    assert(!relation.relation_flags && !capture);
    pico_head_tracking_disconnect(tracking);
    assert(!pico_head_tracking_get(tracking, now, &relation, &capture));
    pico_head_tracking_destroy(&tracking);
    printf("PICO_HEAD_REPLAY_PASS samples=%u orientations=%u\n", samples, orientations);
}
