#include "syncboss-imu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

static void le(unsigned char *data, uint64_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        data[i] = (unsigned char)(value >> (8 * i));
}

static void reject(const unsigned char *data, size_t length, unsigned version)
{
    struct syncboss_imu_parameters parameters = {16.384f, 4096, 25, 128, false};
    struct syncboss_imu_direct_record record, before;
    memset(&record, 0xa7, sizeof(record));
    memcpy(&before, &record, sizeof(record));
    assert(!syncboss_imu_decode_direct(data, length, version, &parameters, &record));
    assert(memcmp(&before, &record, sizeof(record)) == 0);
    ++checks;
}

int main(void)
{
    struct syncboss_imu_parameters parameters = {16.384f, 4096, 25, 128, false};
    unsigned char data[104];
    for (unsigned header_version = 2; header_version <= 3; ++header_version) {
        unsigned header_length = header_version == 2 ? 12 : 21;
        for (unsigned version = 1; version <= 4; ++version) {
            for (unsigned alignment = 0; alignment < 8; ++alignment) {
                memset(data, 0, sizeof(data));
                le(data, 104, 4);
                le(data + 8, 80, 4);
                le(data + 12, UINT32_MAX, 4);
                le(data + 16, 2000000, 8);
                data[24] = header_version;
                data[25] = header_length;
                data[27] = 1;
                le(data + 28, (uint64_t)-100, 8);
                if (header_version == 3) {
                    data[36] = 2;
                    le(data + 37, INT64_MAX, 8);
                }
                unsigned char *payload = data + 24 + header_length;
                le(payload, version >= 3 ? 200ULL << 16 : 200, 8);
                if (version == 2 || version == 3) {
                    le(payload + 8, 4096, 2);
                    le(payload + 20, 128, 2);
                } else {
                    le(payload + 8, 0x3f800000, 4);
                    le(payload + 32, 0x41d00000, 4);
                }
                unsigned char *allocation = malloc(sizeof(data) + alignment);
                assert(allocation);
                memcpy(allocation + alignment, data, sizeof(data));
                struct syncboss_imu_direct_record result;
                assert(syncboss_imu_decode_direct(allocation + alignment, sizeof(data), version,
                                                  &parameters, &result));
                assert(result.timestamp_ns == 100000 && result.delivery_ns == 2000000);
                assert(result.counter == UINT32_MAX && result.sample.timestamp_ticks == 200);
                assert(result.sample.acceleration_m_s2[0] == 9.80665f);
                assert(result.sample.temperature_c == 26 && result.sample.device_id == 0);
                for (size_t length = 0; length < sizeof(data); ++length)
                    reject(allocation + alignment, length, version);
                free(allocation);
                ++checks;
            }
            const unsigned offsets[] = {0, 8, 24, 25, 26, 27};
            for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
                unsigned char saved = data[offsets[i]];
                data[offsets[i]] = 0xff;
                reject(data, sizeof(data), version);
                data[offsets[i]] = saved;
            }
            le(data + 12, 0, 4);
            reject(data, sizeof(data), version);
            le(data + 12, 1, 4);
            le(data + 16, UINT64_MAX, 8);
            reject(data, sizeof(data), version);
            le(data + 16, 0, 8);
            le(data + 28, (uint64_t)-201, 8);
            reject(data, sizeof(data), version);
            le(data + 28, INT64_MAX, 8);
            reject(data, sizeof(data), version);
            le(data + 28, 0, 8);
            if (version >= 3) {
                data[24 + header_length] |= 1;
                reject(data, sizeof(data), version);
            }
        }
    }
    reject(data, 105, 2);
    reject(NULL, 104, 2);
    reject(data, 104, 0);
    assert(!syncboss_imu_decode_direct(data, 104, 2, &parameters, NULL));
    printf("%u direct-channel framing and clock checks passed\n", checks);
    return 0;
}
