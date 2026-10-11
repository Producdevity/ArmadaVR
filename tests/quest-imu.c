#include "quest-imu.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const struct imu_temperature_offset accel_offsets[] = {
    {0, {0, 0, 0}}, {40, {.4f, -.8f, 1.2f}},
};
static const struct imu_temperature_offset gyro_offsets[] = {
    {0, {0, 0, 0}}, {40, {4, -2, .8f}},
};

static struct quest_imu_config configuration(void)
{
    return (struct quest_imu_config){
        .format = 2, .parameters = {16.384f, 4096, 25, 128, false},
        .max_age_ns = 5000000, .frame = IMU_FRAME_ACCELEROMETER,
        .accelerometer = {0, -3, 0, 2, 0, 0, 0, 0, 4},
        .gyroscope = {1, .25, 0, 0, 2, .125, -.1, 0, 3},
        .device_from_imu = {1, 0, 0, 0, 0, -1, 0, 1, 0},
        .accelerometer_offsets = accel_offsets, .gyroscope_offsets = gyro_offsets,
        .accelerometer_offset_count = 2, .gyroscope_offset_count = 2,
    };
}

static void put(unsigned char *bytes, unsigned count, uint64_t value)
{
    for (unsigned i = 0; i < count; ++i)
        bytes[i] = (unsigned char)(value >> (8 * i));
}

static void put_float(unsigned char *bytes, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put(bytes, 4, bits);
}

static int64_t record(unsigned char bytes[104], unsigned format, uint32_t counter,
                       uint64_t ticks, const int raw[7])
{
    memset(bytes, 0, 104);
    int64_t delivery = (int64_t)(ticks + 50) * 1000 + 100000;
    put(bytes, 4, 104); put(bytes + 8, 4, 80); put(bytes + 12, 4, counter);
    put(bytes + 16, 8, (uint64_t)delivery);
    bytes[24] = 2; bytes[25] = 12; bytes[27] = 1;
    put(bytes + 28, 8, 50);
    put(bytes + 36, 8, format >= 3 ? ticks << 16 : ticks);
    if (format == 2 || format == 3) {
        for (unsigned i = 0; i < 7; ++i)
            put(bytes + 44 + 2 * i, 2, (uint16_t)raw[i]);
    } else {
        for (unsigned i = 0; i < 3; ++i) {
            put_float(bytes + 44 + 4 * i, (float)raw[i] / 4096);
            put_float(bytes + 56 + 4 * i, (float)raw[i + 3] / 16.384f);
        }
        put_float(bytes + 68, 25 + (float)raw[6] / 128);
    }
    return delivery;
}

static void close_to(double actual, double expected)
{
    assert(isfinite(actual));
    assert(fabs(actual - expected) <= 2e-5 * fmax(1, fabs(expected)));
}

static void rejected(struct quest_imu *imu, const unsigned char bytes[104], size_t length,
                      int64_t now, enum syncboss_imu_stream_status expected)
{
    struct quest_imu before = *imu;
    struct syncboss_imu_direct_record result, saved;
    memset(&result, 0x5a, sizeof(result)); saved = result;
    uint32_t dropped = 123;
    assert(quest_imu_process_direct(imu, bytes, length, now, &result, &dropped) == expected);
    assert(memcmp(&result, &saved, sizeof(result)) == 0 && dropped == 123);
    assert(memcmp(imu, &before, sizeof(before)) == 0);
}

int main(void)
{
    unsigned cases = 0;
    for (unsigned format = 1; format <= 4; ++format) {
        for (unsigned frame = 0; frame < 2; ++frame) {
            struct quest_imu_config config = configuration();
            config.format = format; config.frame = (enum imu_rectification_frame)frame;
            struct quest_imu imu = {0};
            assert(quest_imu_init(&imu, &config));
            config.accelerometer[0] = NAN;
            config.parameters.lsb_per_g = 0;
            unsigned char bytes[104];
            for (unsigned i = 0; i < 1024; ++i) {
                int raw[] = {(int)i - 512, 1024 - (int)i, (int)i * 2 - 2048,
                             400 - (int)i, (int)i - 300, (int)i / 2, (int)i - 512};
                uint64_t ticks = 1000 + (uint64_t)i * 1250;
                uint32_t counter = i * 2 + 1;
                int64_t now = record(bytes, format, counter, ticks, raw);
                struct syncboss_imu_direct_record result;
                uint32_t dropped;
                assert(quest_imu_process_direct(&imu, bytes, sizeof(bytes), now,
                                                &result, &dropped) == SYNCBOSS_IMU_ACCEPTED);
                assert(dropped == (i ? 1 : 0));
                assert(result.counter == counter && result.sample.timestamp_ticks == ticks);
                assert(result.timestamp_ns == (int64_t)(ticks + 50) * 1000);
                assert(result.delivery_ns == now && result.sample.device_id == 0);
                double temperature = 25 + (double)raw[6] / 128;
                close_to(result.sample.temperature_c, temperature);
                double a[] = {raw[0] * 9.80665 / 4096 - temperature * .01,
                              raw[1] * 9.80665 / 4096 + temperature * .02,
                              raw[2] * 9.80665 / 4096 - temperature * .03};
                double g[] = {raw[3] / 16.384 * .017453292519943295 - temperature * .1,
                              raw[4] / 16.384 * .017453292519943295 + temperature * .05,
                              raw[5] / 16.384 * .017453292519943295 - temperature * .02};
                double corrected_a[] = {-3 * a[1], 2 * a[0], 4 * a[2]};
                double corrected_g[] = {g[0] + .25 * g[1], 2 * g[1] + .125 * g[2], -.1 * g[0] + 3 * g[2]};
                double device_a[] = {corrected_a[0], -corrected_a[2], corrected_a[1]};
                double device_g[] = {corrected_g[0], -corrected_g[2], corrected_g[1]};
                if (frame == IMU_FRAME_ACCELEROMETER) {
                    double first = corrected_a[0];
                    corrected_a[0] = corrected_a[1]; corrected_a[1] = -first;
                    first = corrected_g[0];
                    corrected_g[0] = corrected_g[1]; corrected_g[1] = -first;
                }
                for (unsigned row = 0; row < 3; ++row) {
                    close_to(result.sample.acceleration_m_s2[row], corrected_a[row]);
                    close_to(result.sample.angular_velocity_rad_s[row], corrected_g[row]);
                    double da = 0, dg = 0;
                    for (unsigned col = 0; col < 3; ++col) {
                        da += imu.rectification.device_from_imu[row * 3 + col] * result.sample.acceleration_m_s2[col];
                        dg += imu.rectification.device_from_imu[row * 3 + col] * result.sample.angular_velocity_rad_s[col];
                    }
                    close_to(da, device_a[row]); close_to(dg, device_g[row]);
                }
                rejected(&imu, bytes, sizeof(bytes), now, SYNCBOSS_IMU_OUT_OF_ORDER);
                rejected(&imu, bytes, sizeof(bytes), now + 5000000, SYNCBOSS_IMU_STALE);
                rejected(&imu, bytes, sizeof(bytes), now - 1, SYNCBOSS_IMU_INVALID);
                ++cases;
            }
        }
    }
    struct quest_imu_config config = configuration();
    struct quest_imu imu = {0};
    unsigned char bytes[104];
    int raw[] = {1, 2, 3, 4, 5, 6, 0};
    int64_t now = record(bytes, 2, UINT32_MAX, 10000, raw);
    rejected(&imu, bytes, sizeof(bytes), now, SYNCBOSS_IMU_INVALID);
    assert(quest_imu_init(&imu, &config));
    struct syncboss_imu_direct_record result;
    uint32_t dropped;
    assert(quest_imu_process_direct(&imu, bytes, sizeof(bytes), now, &result, &dropped) == 0);
    now = record(bytes, 2, 1, 11250, raw);
    assert(quest_imu_process_direct(&imu, bytes, sizeof(bytes), now, &result, &dropped) == 0 && dropped == 0);
    for (unsigned size = 0; size < 104; ++size)
        rejected(&imu, bytes, size, now, SYNCBOSS_IMU_INVALID);
    rejected(&imu, bytes, 105, now, SYNCBOSS_IMU_INVALID);
    rejected(&imu, NULL, 104, now, SYNCBOSS_IMU_INVALID);
    struct quest_imu saved = imu;
    assert(!quest_imu_init(NULL, &config) && !quest_imu_init(&imu, NULL));
    for (unsigned bad = 0; bad < 12; ++bad) {
        struct quest_imu_config invalid = config;
        switch (bad) {
        case 0: invalid.format = 0; break;
        case 1: invalid.parameters.lsb_per_g = 0; break;
        case 2: invalid.parameters.lsb_per_dps = NAN; break;
        case 3: invalid.parameters.lsb_per_c = .5f; break;
        case 4: invalid.max_age_ns = 0; break;
        case 5: invalid.frame = (enum imu_rectification_frame)9; break;
        case 6: invalid.accelerometer[0] = NAN; break;
        case 7: invalid.gyroscope[0] = INFINITY; break;
        case 8: invalid.device_from_imu[0] = -1; break;
        case 9: invalid.accelerometer_offsets = NULL; break;
        case 10: invalid.gyroscope_offset_count = 0; break;
        case 11: invalid.accelerometer_offset_count = 1025; break;
        }
        assert(!quest_imu_init(&imu, &invalid));
        assert(memcmp(&imu, &saved, sizeof(imu)) == 0);
    }
    assert(quest_imu_init(&imu, &config));
    assert(quest_imu_process_direct(&imu, bytes, sizeof(bytes), now, &result, &dropped) == 0);
    static const struct imu_temperature_offset zero = {0, {0, 0, 0}};
    config.accelerometer_offsets = config.gyroscope_offsets = &zero;
    config.accelerometer_offset_count = config.gyroscope_offset_count = 1;
    config.frame = IMU_FRAME_ORIGINAL;
    for (unsigned which = 0; which < 2; ++which) {
        for (unsigned i = 0; i < 9; ++i) {
            config.accelerometer[i] = i % 4 ? 0 : which ? 1 : FLT_MAX;
            config.gyroscope[i] = i % 4 ? 0 : which ? FLT_MAX : 1;
        }
        assert(quest_imu_init(&imu, &config));
        int large[] = {32767, 32767, 32767, 32767, 32767, 32767, 0};
        now = record(bytes, 2, 1, 10000, large);
        rejected(&imu, bytes, sizeof(bytes), now, SYNCBOSS_IMU_INVALID);
        int zero_raw[7] = {0};
        record(bytes, 2, 1, 10000, zero_raw);
        assert(quest_imu_process_direct(&imu, bytes, sizeof(bytes), now, &result, &dropped) == 0);
    }
    printf("%u calibrated IMU stream cases and rejection checks passed\n", cases);
    return 0;
}
