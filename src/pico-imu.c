#include "pico-imu.h"
#include <limits.h>
#include <math.h>
#include <string.h>

static uint32_t le32(const unsigned char *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t le64(const unsigned char *data)
{
    return (uint64_t)le32(data) | (uint64_t)le32(data + 4) << 32;
}

bool pico_imu_init(struct pico_imu *imu, const struct ssc_clock *clock,
                   int32_t acceleration_token, int32_t gyroscope_token, int64_t max_age_ns)
{
    if (!imu || !clock || !clock->initialized || !clock->counter_hz ||
        clock->host_ns < 0 || clock->uncertainty_ns < 2 ||
        acceleration_token <= 0 || gyroscope_token <= 0 ||
        acceleration_token == gyroscope_token || max_age_ns <= 0)
        return false;
    *imu = (struct pico_imu){
        .clock = *clock, .tokens = {acceleration_token, gyroscope_token},
        .max_age_ns = max_age_ns, .initialized = true,
    };
    return true;
}

static void discard(struct pico_imu *imu, unsigned index, bool unpaired)
{
    if (!imu->count[index])
        return;
    imu->count[index]--;
    memmove(imu->records[index], imu->records[index] + 1,
            imu->count[index] * sizeof(imu->records[index][0]));
    if (unpaired && imu->unpaired != UINT64_MAX)
        imu->unpaired++;
}

enum pico_imu_status pico_imu_process(struct pico_imu *imu, const void *record,
                                     size_t length, int64_t now_ns,
                                     struct pico_imu_sample *sample, uint32_t *dropped)
{
    if (!imu || !imu->initialized || !record || length != 104 || !sample || !dropped || now_ns < 0)
        return PICO_IMU_INVALID;
    const unsigned char *data = record;
    uint32_t type = le32(data + 8), sequence = le32(data + 12);
    uint64_t raw_ns = le64(data + 16);
    if (le32(data) != 104 || (type != 35 && type != 16) || !sequence ||
        !raw_ns || raw_ns > INT64_MAX)
        return PICO_IMU_INVALID;
    unsigned index = type == 35 ? 0 : 1;
    if (le32(data + 4) != (uint32_t)imu->tokens[index])
        return PICO_IMU_INVALID;
    struct pico_imu_record value = {.counter_ns = (int64_t)raw_ns};
    for (unsigned i = 0; i < 6; i++) {
        uint32_t bits = le32(data + 24 + i * 4);
        float number;
        _Static_assert(sizeof(number) == 4, "32-bit sensor float required");
        memcpy(&number, &bits, sizeof(number));
        if (!isfinite(number))
            return PICO_IMU_INVALID;
        if (i < 3)
            value.vector[i] = number;
        else
            value.vendor_aux[i - 3] = number;
    }
    uint64_t distance = 1;
    if (imu->sequence) {
        distance = sequence > imu->sequence ? sequence - imu->sequence :
                   (uint64_t)UINT32_MAX - imu->sequence + sequence;
        if (!distance || distance > UINT32_MAX / 2 || raw_ns <= (uint64_t)imu->last_ns[index])
            return PICO_IMU_OUT_OF_ORDER;
    }
    __int128 counter_ns = (__int128)imu->clock.counter_ticks * 1000000000 / imu->clock.counter_hz;
    __int128 mapped = imu->clock.host_ns + (__int128)raw_ns - counter_ns;
    __int128 earliest = mapped - imu->clock.uncertainty_ns;
    __int128 latest = mapped + imu->clock.uncertainty_ns;
    if (earliest < 0 || latest > now_ns || (__int128)now_ns - earliest > imu->max_age_ns)
        return PICO_IMU_STALE;
    struct pico_imu next = *imu;
    if (distance != 1) {
        for (unsigned sensor = 0; sensor < 2; sensor++)
            while (next.count[sensor])
                discard(&next, sensor, true);
    }
    next.sequence = sequence;
    next.last_ns[index] = (int64_t)raw_ns;
    if (next.count[index] == PICO_IMU_PENDING)
        discard(&next, index, true);
    next.records[index][next.count[index]++] = value;
    enum pico_imu_status status = PICO_IMU_NEEDS_PAIR;
    while (next.count[0] && next.count[1]) {
        const struct pico_imu_record *accel = &next.records[0][0], *gyro = &next.records[1][0];
        if (accel->counter_ns == gyro->counter_ns) {
            struct pico_imu_sample output = {
                .counter_ns = (int64_t)raw_ns, .monotonic_ns = (int64_t)mapped,
                .uncertainty_ns = imu->clock.uncertainty_ns,
            };
            memcpy(output.acceleration, accel->vector, sizeof(output.acceleration));
            memcpy(output.angular_velocity, gyro->vector, sizeof(output.angular_velocity));
            memcpy(output.acceleration_aux, accel->vendor_aux, sizeof(output.acceleration_aux));
            memcpy(output.gyroscope_aux, gyro->vendor_aux, sizeof(output.gyroscope_aux));
            *sample = output;
            discard(&next, 0, false);
            discard(&next, 1, false);
            status = PICO_IMU_ACCEPTED;
            break;
        }
        discard(&next, accel->counter_ns < gyro->counter_ns ? 0 : 1, true);
    }
    *imu = next;
    *dropped = (uint32_t)(distance - 1);
    return status;
}
