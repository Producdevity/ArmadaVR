#include "pico-imu.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(expression) do { assert(expression); checks++; } while (0)

static void put32(unsigned char *data, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) data[i] = value >> (8 * i);
}

static void put64(unsigned char *data, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) data[i] = value >> (8 * i);
}

static void event(unsigned char data[104], uint32_t type, uint32_t sequence, int64_t ns)
{
    memset(data, 0, 104);
    put32(data, 104);
    put32(data + 4, type == 35 ? 351 : 161);
    put32(data + 8, type);
    put32(data + 12, sequence);
    put64(data + 16, ns);
    for (unsigned i = 0; i < 6; i++) {
        float value = (float)(type + i);
        uint32_t bits;
        memcpy(&bits, &value, 4);
        put32(data + 24 + 4 * i, bits);
    }
}

static struct pico_imu initialize(void)
{
    struct ssc_clock clock;
    CHECK(ssc_clock_init(&clock, 192000000, 19200000, 6999999950, 7000000050, 1000));
    clock.uncertainty_ns += 53;
    struct pico_imu state;
    CHECK(pico_imu_init(&state, &clock, 351, 161, 100000000));
    return state;
}

static void rejection(struct pico_imu *state, unsigned char *record, size_t size,
                      int64_t now, enum pico_imu_status expected)
{
    struct pico_imu before = *state;
    struct pico_imu_sample output, sentinel;
    memset(&output, 0xa5, sizeof(output));
    sentinel = output;
    uint32_t dropped = 123;
    CHECK(pico_imu_process(state, record, size, now, &output, &dropped) == expected);
    CHECK(memcmp(&before, state, sizeof(before)) == 0);
    CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
    CHECK(dropped == 123);
}

static void unit_tests(void)
{
    struct pico_imu state = initialize();
    unsigned char record[104];
    struct pico_imu_sample output = {0};
    uint32_t dropped = 123;
    const int64_t capture = 10100000000, now = 7110000000;
    event(record, 35, 1, capture);
    CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_NEEDS_PAIR);
    CHECK(dropped == 0 && output.monotonic_ns == 0);
    event(record, 16, 2, capture);
    CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_ACCEPTED);
    CHECK(output.monotonic_ns == 7100000000 && output.counter_ns == capture);
    CHECK(output.acceleration[2] == 37 && output.angular_velocity[2] == 18);
    CHECK(output.acceleration_aux[0] == 38 && output.gyroscope_aux[0] == 19);
    CHECK(output.uncertainty_ns == 105 && dropped == 0);
    rejection(&state, record, 104, now, PICO_IMU_OUT_OF_ORDER);
    event(record, 16, 3, capture);
    rejection(&state, record, 104, now, PICO_IMU_OUT_OF_ORDER);
    event(record, 16, 1, capture + 1);
    rejection(&state, record, 104, now, PICO_IMU_OUT_OF_ORDER);

    state = initialize();
    for (unsigned mode = 0; mode < 10; mode++) {
        event(record, 35, 1, capture);
        switch (mode) {
        case 0: put32(record, 103); break;
        case 1: put32(record + 4, 161); break;
        case 2: put32(record + 8, 4); break;
        case 3: put32(record + 12, 0); break;
        case 4: put64(record + 16, 0); break;
        case 5: put64(record + 16, UINT64_MAX); break;
        case 6: put32(record + 24, 0x7fc00000); break;
        case 7: put32(record + 44, 0x7f800000); break;
        default: break;
        }
        rejection(&state, record, mode == 8 ? 103 : 104, mode == 9 ? -1 : now, PICO_IMU_INVALID);
    }
    event(record, 35, 1, capture);
    rejection(&state, record, 104, 7099999999, PICO_IMU_STALE);
    rejection(&state, record, 104, 7200000000, PICO_IMU_STALE);
    event(record, 35, 1, 1);
    rejection(&state, record, 104, now, PICO_IMU_STALE);

    state = initialize();
    event(record, 16, UINT32_MAX, capture);
    CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_NEEDS_PAIR);
    event(record, 35, 1, capture);
    CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_ACCEPTED);
    CHECK(dropped == 0);

    state = initialize();
    event(record, 35, 1, capture);
    CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_NEEDS_PAIR);
    event(record, 16, 3, capture);
    CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_NEEDS_PAIR);
    CHECK(dropped == 1);
    event(record, 35, 4, capture + 1000);
    CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_NEEDS_PAIR);
    event(record, 16, 5, capture + 2000);
    CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_NEEDS_PAIR);
    event(record, 35, 6, capture + 2000);
    CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_ACCEPTED);
    CHECK(output.counter_ns == capture + 2000);

    state = initialize();
    for (unsigned i = 0; i < 2; i++) {
        event(record, 35, i + 1, capture + i * 1000);
        CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_NEEDS_PAIR);
    }
    for (unsigned i = 0; i < 2; i++) {
        event(record, 16, i + 3, capture + i * 1000);
        CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_ACCEPTED);
        CHECK(output.counter_ns == capture + i * 1000);
    }
    CHECK(state.unpaired == 0 && !state.count[0] && !state.count[1]);

    state = initialize();
    for (unsigned i = 0; i < PICO_IMU_PENDING + 2; i++) {
        event(record, 16, i + 1, capture + i * 1000);
        CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) == PICO_IMU_NEEDS_PAIR);
    }
    CHECK(state.unpaired == 2 && state.count[1] == PICO_IMU_PENDING);
    for (unsigned i = 0; i < PICO_IMU_PENDING + 2; i++) {
        event(record, 35, PICO_IMU_PENDING + i + 3, capture + i * 1000);
        CHECK(pico_imu_process(&state, record, 104, now, &output, &dropped) ==
               (i < 2 ? PICO_IMU_NEEDS_PAIR : PICO_IMU_ACCEPTED));
    }
    CHECK(state.unpaired == 4 && !state.count[0] && !state.count[1]);

    struct pico_imu before = state;
    struct ssc_clock clock = state.clock;
    CHECK(!pico_imu_init(&state, &clock, 351, 351, 100));
    CHECK(!pico_imu_init(&state, &clock, 0, 161, 100));
    CHECK(!pico_imu_init(&state, &clock, 351, 161, 0));
    clock.counter_hz = 0;
    CHECK(!pico_imu_init(&state, &clock, 351, 161, 100));
    CHECK(memcmp(&before, &state, sizeof(state)) == 0);
    CHECK(pico_imu_init(&state, &before.clock, 351, 161, 100000000));
    CHECK(!state.sequence && !state.count[0] && !state.count[1]);
    printf("PICO_IMU_UNIT_PASS checks=%u\n", checks);
}

static int replay(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) return 1;
    uint64_t ticks;
    uint32_t frequency;
    int64_t before, after, now;
    CHECK(fscanf(file, "CLOCK %" SCNu64 " %" SCNu32 " %" SCNd64 " %" SCNd64 " %" SCNd64 "\n",
                 &ticks, &frequency, &before, &after, &now) == 5);
    struct ssc_clock clock;
    CHECK(ssc_clock_init(&clock, ticks, frequency, before, after, 1000000));
    clock.uncertainty_ns += (1000000000 + (uint64_t)frequency - 1) / frequency;
    struct pico_imu state;
    CHECK(pico_imu_init(&state, &clock, 351, 161, 3000000000));
    unsigned char record[104];
    uint32_t type, sequence, count = 0;
    int64_t timestamp;
    float values[6];
    int fields;
    while ((fields = fscanf(file, "SAMPLE %" SCNu32 " %" SCNu32 " %" SCNd64
                            " %f %f %f %f %f %f\n", &sequence, &type, &timestamp,
                            &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])) != EOF) {
        CHECK(fields == 9);
        event(record, type, sequence, timestamp);
        for (unsigned i = 0; i < 6; i++) {
            uint32_t bits;
            memcpy(&bits, &values[i], 4);
            put32(record + 24 + i * 4, bits);
        }
        struct pico_imu_sample sample;
        uint32_t dropped;
        enum pico_imu_status status = pico_imu_process(&state, record, 104, now, &sample, &dropped);
        CHECK(status == PICO_IMU_ACCEPTED || status == PICO_IMU_NEEDS_PAIR);
        CHECK(dropped == 0);
        if (status == PICO_IMU_ACCEPTED) {
            printf("PAIR %" PRId64 " %" PRId64, sample.counter_ns, sample.monotonic_ns);
            const float *vectors[] = {sample.acceleration, sample.angular_velocity,
                                      sample.acceleration_aux, sample.gyroscope_aux};
            for (unsigned vector = 0; vector < 4; vector++)
                for (unsigned axis = 0; axis < 3; axis++) printf(" %.9g", vectors[vector][axis]);
            puts("");
            count++;
        }
    }
    CHECK(!ferror(file) && !fclose(file));
    printf("PICO_IMU_REPLAY_PASS pairs=%u\n", count);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2) return replay(argv[1]);
    if (argc != 1) return 2;
    unit_tests();
    return 0;
}
