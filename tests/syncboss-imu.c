#include "syncboss-imu.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

static void unhex(const char *text, void *output)
{
    unsigned char *bytes = output;
    while (*text) {
        unsigned byte;
        assert(sscanf(text, "%2x", &byte) == 1);
        *bytes++ = (unsigned char)byte;
        text += 2;
    }
}

static void rejected(const void *data, size_t size, unsigned version,
                     const struct syncboss_imu_parameters *parameters)
{
    struct syncboss_imu_sample before, result;
    memset(&before, 0x5a, sizeof(before));
    memcpy(&result, &before, sizeof(result));
    assert(!syncboss_imu_decode(data, size, version, parameters, &result));
    assert(memcmp(&before, &result, sizeof(result)) == 0);
    ++checks;
}

int main(void)
{
    _Static_assert(offsetof(struct syncboss_imu_sample, angular_velocity_rad_s) == 32,
                   "Fixture output layout");
    const char *floating = "ffdebc9a785634120000803f000000c00000003f000034430000b4c20000b4430000aa41";
    const char *integer = "ffdebc9a785634120080ffffff7f004000c000007bff";
    const struct {
        unsigned version;
        bool compensated;
        const char *expected;
    } vectors[] = {
        {1, false, "0000000000000000ffdebc9a785634120000aa410ae81c410ae89cc10ae89c40db0f4940db0fc9bfdb0fc940"},
        {2, false, "0000000000000000ffdebc9a785634127cf097410ae81cc30ae89cbbd0e61c43797d8b41797d8bc100000000"},
        {2, true, "0000000000000000ffdebc9a785634127cf0974160f01cc360f09cbb71eb1c4383838b41b0828bc100000000"},
        {3, false, "0f00000000000000bc9a7856341200007cf097410ae81cc30ae89cbbd0e61c43797d8b41797d8bc100000000"},
        {3, true, "0f00000000000000bc9a7856341200007cf0974160f01cc360f09cbb71eb1c4383838b41b0828bc100000000"},
        {4, false, "0f00000000000000bc9a7856341200000000aa410ae81c410ae89cc10ae89c40db0f4940db0fc9bfdb0fc940"},
    };
    struct syncboss_imu_parameters parameters = {16.4f, 2048, 20.9f, 132.48f, false};
    unsigned char data[253] = {0};
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(*vectors); ++i) {
        bool packed = vectors[i].version == 2 || vectors[i].version == 3;
        size_t length = packed ? 22 : 36;
        unsigned char expected[44];
        unhex(packed ? integer : floating, data);
        unhex(vectors[i].expected, expected);
        parameters.temperature_compensation = vectors[i].compensated;
        for (unsigned alignment = 0; alignment < 8; ++alignment) {
            unsigned char *allocation = malloc(length + alignment);
            assert(allocation);
            memcpy(allocation + alignment, data, length);
            struct syncboss_imu_sample sample;
            assert(syncboss_imu_decode(allocation + alignment, length, vectors[i].version,
                                       &parameters, &sample));
            assert(memcmp(&sample, expected, sizeof(expected)) == 0);
            for (size_t truncated = 0; truncated < length; ++truncated)
                rejected(allocation + alignment, truncated, vectors[i].version, &parameters);
            free(allocation);
            ++checks;
        }
        struct syncboss_imu_sample padded;
        assert(syncboss_imu_decode(data, 252, vectors[i].version, &parameters, &padded));
        assert(memcmp(&padded, expected, sizeof(expected)) == 0);
        rejected(data, 253, vectors[i].version, &parameters);
        ++checks;
    }
    rejected(NULL, 36, 1, &parameters);
    rejected(data, 36, 0, &parameters);
    rejected(data, 36, 5, &parameters);
    rejected(data, 36, 2, NULL);
    assert(!syncboss_imu_decode(data, 36, 1, &parameters, NULL));
    const float invalid[] = {0, -1, NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        struct syncboss_imu_parameters bad = parameters;
        bad.lsb_per_g = invalid[i];
        rejected(data, 22, 2, &bad);
        bad = parameters;
        bad.lsb_per_dps = invalid[i];
        rejected(data, 22, 2, &bad);
        bad = parameters;
        bad.lsb_per_c = invalid[i];
        rejected(data, 22, 3, &bad);
    }
    const float invalid_temperature[] = {0.99f, 0x1p31f, FLT_MAX};
    for (unsigned i = 0; i < sizeof(invalid_temperature) / sizeof(*invalid_temperature); ++i) {
        struct syncboss_imu_parameters bad = parameters;
        bad.lsb_per_c = invalid_temperature[i];
        rejected(data, 22, 2, &bad);
    }
    const float invalid_offsets[] = {NAN, INFINITY, -INFINITY, 0x1p31f, -FLT_MAX};
    for (unsigned i = 0; i < sizeof(invalid_offsets) / sizeof(*invalid_offsets); ++i) {
        struct syncboss_imu_parameters bad = parameters;
        bad.offset_c = invalid_offsets[i];
        rejected(data, 22, 3, &bad);
    }
    const unsigned char nan_le[] = {0, 0, 0xc0, 0x7f};
    const unsigned char infinity_le[] = {0, 0, 0x80, 0x7f};
    for (unsigned offset = 8; offset < 36; offset += 4) {
        unhex(floating, data);
        memcpy(data + offset, nan_le, 4);
        rejected(data, 36, 1, NULL);
        memcpy(data + offset, infinity_le, 4);
        rejected(data, 36, 4, NULL);
    }
    unhex(integer, data);
    parameters.lsb_per_g = FLT_MIN;
    rejected(data, 22, 2, &parameters);
    printf("%u IMU conversion and rejection checks passed\n", checks);
    return 0;
}
