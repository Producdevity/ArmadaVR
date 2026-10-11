#include "syncboss-imu.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static const struct syncboss_imu_parameters parameters = {16.384f, 4096, 25, 126.8f, false};

static void put_u64(unsigned char *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

static size_t make_record(unsigned char *data, unsigned header, unsigned version,
                          uint64_t ticks, int64_t offset)
{
    unsigned length = header == 2 ? 12 : 21;
    unsigned payload = version == 2 || version == 3 ? 22 : 36;
    memset(data, 0, 277);
    data[0] = (unsigned char)header;
    data[1] = (unsigned char)length;
    data[3] = 1;
    put_u64(data + 4, (uint64_t)offset);
    data[length] = 80;
    data[length + 1] = 237;
    data[length + 2] = (unsigned char)payload;
    put_u64(data + length + 3, version >= 3 ? ticks << 16 : ticks);
    if (version == 2 || version == 3)
        data[length + 3 + 13] = 16; /* 4096 raw Z acceleration: one g. */
    else {
        data[length + 3 + 18] = 128;
        data[length + 3 + 19] = 63;
    }
    return length + 3 + payload;
}

static void rejected(const void *data, size_t length, unsigned version)
{
    struct syncboss_imu_record before, result;
    memset(&before, 0x5a, sizeof(before));
    memcpy(&result, &before, sizeof(result));
    assert(!syncboss_imu_decode_fifo(data, length, version, &parameters, &result));
    assert(!memcmp(&before, &result, sizeof(result)));
    ++checks;
}

static void clock_case(uint64_t ticks, int64_t offset, bool accepted, int64_t expected)
{
    unsigned char data[277];
    size_t length = make_record(data, 2, 2, ticks, offset);
    if (!accepted) {
        rejected(data, length, 2);
        return;
    }
    struct syncboss_imu_record record;
    assert(syncboss_imu_decode_fifo(data, length, 2, &parameters, &record));
    assert(record.timestamp_ns == expected && record.sample.timestamp_ticks == ticks);
    ++checks;
}

int main(void)
{
    unsigned char data[277];
    struct syncboss_imu_record record;
    for (unsigned header = 2; header <= 3; ++header) {
        for (unsigned version = 1; version <= 4; ++version) {
            size_t length = make_record(data, header, version, 1200000, -200000);
            for (unsigned alignment = 0; alignment < 8; ++alignment) {
                unsigned char *allocation = malloc(length + alignment);
                assert(allocation);
                memcpy(allocation + alignment, data, length);
                assert(syncboss_imu_decode_fifo(allocation + alignment, length, version,
                                                &parameters, &record));
                assert(record.timestamp_ns == 1000000000 && record.sequence == 237);
                assert(record.sample.timestamp_ticks == 1200000 && record.sample.device_id == 0);
                assert(record.sample.acceleration_m_s2[2] == 9.80665f);
                assert(record.sample.acceleration_m_s2[0] == 0);
                ++checks;
                free(allocation);
            }
            for (size_t cut = 0; cut < length; ++cut) {
                unsigned char *allocation = malloc(cut ? cut : 1);
                assert(allocation);
                memcpy(allocation, data, cut);
                rejected(allocation, cut, version);
                free(allocation);
            }
            rejected(data, length + 1, version);
            data[data[1] + 2] = 252;
            assert(syncboss_imu_decode_fifo(data, data[1] + 255u, version, &parameters, &record));
            ++checks;
            data[data[1] + 2] = 253;
            rejected(data, data[1] + 256u, version);
        }
    }
    size_t length = make_record(data, 3, 2, 1, 0);
    rejected(NULL, length, 2);
    assert(!syncboss_imu_decode_fifo(data, length, 2, &parameters, NULL));
    assert(!syncboss_imu_decode_fifo(data, length, 2, NULL, &record));
    rejected(data, length, 0);
    rejected(data, length, 5);
    rejected(data, 277, 2);
    for (unsigned value = 0; value < 256; ++value) {
        data[0] = (unsigned char)value;
        if (value != 3)
            rejected(data, length, 2);
        data[0] = 3;
        data[1] = (unsigned char)value;
        if (value != 21)
            rejected(data, length, 2);
        data[1] = 21;
        data[2] = (unsigned char)value;
        if (value != 0)
            rejected(data, length, 2);
        data[2] = 0;
        data[3] = (unsigned char)value;
        if (value != 1)
            rejected(data, length, 2);
        data[3] = 1;
        data[12] = (unsigned char)value;
        if (value > 2)
            rejected(data, length, 2);
        else {
            assert(syncboss_imu_decode_fifo(data, length, 2, &parameters, &record));
            ++checks;
        }
        data[12] = 0;
        data[21] = (unsigned char)value;
        if (value != 80)
            rejected(data, length, 2);
        data[21] = 80;
        data[22] = (unsigned char)value;
        assert(syncboss_imu_decode_fifo(data, length, 2, &parameters, &record));
        assert(record.sequence == value);
        ++checks;
    }
    for (unsigned version = 3; version <= 4; ++version) {
        length = make_record(data, 3, version, 1, 0);
        for (unsigned id = 1; id < 16; ++id) {
            data[24] = (unsigned char)id;
            rejected(data, length, version);
        }
    }
    const uint64_t limit = INT64_MAX / 1000;
    clock_case(0, 0, true, 0);
    clock_case(1, -1, true, 0);
    clock_case(1, -2, false, 0);
    clock_case(0, INT64_MIN, false, 0);
    clock_case(UINT64_C(1) << 63, INT64_MIN, true, 0);
    clock_case((UINT64_C(1) << 63) + 1, INT64_MIN, true, 1000);
    clock_case(UINT64_MAX, INT64_MIN, false, 0);
    clock_case(UINT64_MAX, 1, false, 0);
    clock_case(UINT64_MAX, INT64_MAX, false, 0);
    clock_case(0, INT64_MAX, false, 0);
    clock_case(limit, 0, true, (int64_t)limit * 1000);
    clock_case(limit - 1, 1, true, (int64_t)limit * 1000);
    clock_case(limit + 1, -1, true, (int64_t)limit * 1000);
    clock_case(limit, 1, false, 0);
    clock_case(limit + 1, 0, false, 0);
    printf("%u FIFO framing and clock checks passed\n", checks);
    return 0;
}
