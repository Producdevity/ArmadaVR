#include "ssc-imu.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error Assertions must be enabled
#endif

static unsigned checks;
static uint32_t random_state = 0x5149434f;

static uint32_t random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool decode(const void *payload, size_t length, struct ssc_imu_vector *vector)
{
    unsigned char before[sizeof(*vector)];
    memcpy(before, vector, sizeof(*vector));
    bool result = ssc_imu_decode(payload, length, vector);
    if (!result)
        assert(!memcmp(before, vector, sizeof(*vector)));
    else {
        assert(vector->accuracy <= 3);
        for (unsigned i = 0; i < 3; i++) assert(isfinite(vector->values[i]));
    }
    checks++;
    return result;
}

static const unsigned char packed[] = {
    0x0a, 12, 0, 0, 0x80, 0x3f, 0, 0, 0, 0xc0, 0, 0, 0x60, 0x40, 0x10, 3,
};

static void payload_tests(void)
{
    struct ssc_imu_vector vector;
    memset(&vector, 0x5a, sizeof(vector));
    assert(decode(packed, sizeof(packed), &vector));
    assert(vector.values[0] == 1 && vector.values[1] == -2 && vector.values[2] == 3.5f && vector.accuracy == 3);
    unsigned char payload[4200];
    for (unsigned accuracy = 0; accuracy <= 4; accuracy++) {
        memcpy(payload, packed, sizeof(packed)); payload[15] = (unsigned char)accuracy;
        assert(decode(payload, 16, &vector) == (accuracy < 4));
        if (accuracy < 4) assert(vector.accuracy == accuracy);
    }
    unsigned char mixed[] = {0x10, 2, 0x0d, 0, 0, 0x80, 0x3f, 0x0a, 8, 0, 0, 0, 0xc0,
                             0, 0, 0x60, 0x40, 0x98, 6, 0x80, 1, 0x10, 1};
    assert(decode(mixed, sizeof(mixed), &vector) && vector.accuracy == 1);
    assert(vector.values[2] == 3.5f);
    for (size_t length = 0; length < sizeof(packed); length++)
        assert(!decode(packed, length, &vector));
    memcpy(payload + 1, packed, sizeof(packed));
    assert(decode(payload + 1, sizeof(packed), &vector));
    for (unsigned wire = 0; wire < 8; wire++) {
        if (wire == 2) continue;
        memcpy(payload, packed, sizeof(packed)); payload[0] = (unsigned char)(8 + wire);
        assert(!decode(payload, 16, &vector));
    }
    for (unsigned wire = 1; wire < 8; wire++) {
        memcpy(payload, packed, sizeof(packed)); payload[14] = (unsigned char)(16 + wire);
        assert(!decode(payload, 16, &vector));
    }
    for (unsigned length = 0; length < 20; length++) {
        if (length == 12) continue;
        memcpy(payload, packed, sizeof(packed)); payload[1] = (unsigned char)length;
        assert(!decode(payload, 16, &vector));
    }
    const uint32_t bits[] = {0, 0x80000000, 1, 0x80000001, 0x7f7fffff, 0xff7fffff,
                            0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001};
    for (unsigned axis = 0; axis < 3; axis++) {
        for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
            memcpy(payload, packed, sizeof(packed));
            for (unsigned j = 0; j < 4; j++) payload[2 + axis * 4 + j] = (unsigned char)(bits[i] >> (8 * j));
            assert(decode(payload, 16, &vector) == (i < 6));
            if (i < 6) assert(!memcmp(&vector.values[axis], &bits[i], 4));
        }
    }
    const unsigned char suffixes[][12] = {
        {0}, {0xff, 0xff, 0xff, 0xff, 0x1f}, {0x98, 6, 0x80},
        {0x98, 6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2},
        {0x9b, 6}, {0x9c, 6}, {0x99, 6}, {0x9a, 6, 10, 1}, {0x9d, 6, 1},
        {0x0d, 0, 0, 0, 0}, {0x10, 4},
    };
    const unsigned sizes[] = {1, 5, 3, 12, 2, 2, 2, 4, 3, 5, 2};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        memcpy(payload, packed, 16); memcpy(payload + 16, suffixes[i], sizes[i]);
        assert(!decode(payload, 16 + sizes[i], &vector));
    }
    const unsigned char unknown[] = {0x98, 6, 0xff, 1, 0x99, 6, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0x9a, 6, 3, 'a', 'b', 'c', 0x9d, 6, 0, 0, 0, 0};
    memcpy(payload, packed, 16); memcpy(payload + 16, unknown, sizeof(unknown));
    assert(decode(payload, 16 + sizeof(unknown), &vector));
    assert(!decode(NULL, 16, &vector));
    assert(!ssc_imu_decode(packed, 16, NULL));
    assert(!decode(payload, 4097, &vector));
    for (unsigned i = 0; i < 100000; i++) {
        size_t length = random_word() % 80;
        for (size_t j = 0; j < length; j++) payload[j] = (unsigned char)random_word();
        if (i & 1) {
            memcpy(payload, packed, 16); length = 16;
            payload[random_word() % 16] ^= (unsigned char)random_word();
        }
        (void)decode(payload, length, &vector);
    }
}

static bool receive(struct ssc_imu_stream *stream, uint64_t generation, unsigned sensor,
                    uint32_t id, uint64_t high, uint64_t ticks, int64_t now,
                    struct ssc_imu_event *event)
{
    unsigned char before[sizeof(*stream)], output[sizeof(*event)];
    memcpy(before, stream, sizeof(*stream)); memcpy(output, event, sizeof(*event));
    bool ok = ssc_imu_report(stream, generation, id, high, sensor + 2, ticks, packed, sizeof(packed), now, event);
    if (!ok) {
        assert(!memcmp(before, stream, sizeof(*stream)));
        assert(!memcmp(output, event, sizeof(*event)));
    } else assert((unsigned)event->sensor == sensor && event->generation == generation && event->ticks == ticks);
    checks++;
    return ok;
}

static void stream_tests(void)
{
    const uint64_t anchor = UINT64_C(0x123456789abcdef);
    const int64_t now = 1000001000;
    struct ssc_imu_config config = {.suid_high = {1, 1}, .suid_low = {2, 3}, .max_age_ns = 1000000};
    assert(ssc_clock_init(&config.clock, anchor, 19200000, 1000000000, 1000000000, 0));
    struct ssc_imu_stream stream = {0};
    struct ssc_imu_event event;
    memset(&event, 0x5a, sizeof(event));
    assert(!receive(&stream, 0, 0, 1025, 1, anchor, now, &event));
    assert(ssc_imu_begin(&stream, &config) && stream.generation == 1);
    assert(receive(&stream, 1, 0, 1025, 1, anchor + 3, now, &event));
    assert(event.time.timestamp_ns == 1000000156 && event.time.uncertainty_ns == 2);
    assert(receive(&stream, 1, 1, 1025, 1, anchor + 1, now, &event));
    assert(!receive(&stream, 1, 0, 1025, 1, anchor + 3, now, &event));
    assert(!receive(&stream, 1, 0, 1025, 1, anchor + 2, now, &event));
    assert(!receive(&stream, 1, 0, 1025, 2, anchor + 4, now, &event));
    assert(!receive(&stream, 1, 2, 1025, 1, anchor + 4, now, &event));
    assert(!receive(&stream, 1, 0, 1025, 1, anchor + 4, now - 1, &event));
    assert(!receive(&stream, 1, 0, 1025, 1, anchor + 40000, now, &event));
    assert(!receive(&stream, 1, 0, 1025, 1, anchor + 4, now + 2000000, &event));
    for (unsigned id = 0; id < 2048; id++)
        if (id != 1025) assert(!receive(&stream, 1, 0, id, 1, anchor + 4, now, &event));
    assert(receive(&stream, 1, 0, 1025, 1, anchor + 4, now, &event));
    for (unsigned cycle = 0; cycle < 1000; cycle++) {
        uint64_t old = stream.generation;
        ssc_imu_invalidate(&stream);
        assert(!receive(&stream, old, 0, 1025, 1, anchor + 5, now, &event));
        assert(ssc_imu_begin(&stream, &config) && stream.generation == old + 1);
        assert(!receive(&stream, old, 0, 1025, 1, anchor + 5, now, &event));
        assert(receive(&stream, stream.generation, 0, 1025, 1, anchor, now, &event));
        assert(receive(&stream, stream.generation, 1, 1025, 1, anchor, now, &event));
    }
    for (unsigned invalid = 0; invalid < 8; invalid++) {
        struct ssc_imu_config bad = config;
        if (invalid == 0) bad.suid_low[1] = 2;
        if (invalid == 1) bad.suid_low[0] = bad.suid_high[0] = 0;
        if (invalid == 2) bad.suid_low[1] = bad.suid_high[1] = 0;
        if (invalid == 3) bad.max_age_ns = -1;
        if (invalid == 4) bad.clock.initialized = false;
        if (invalid == 5) bad.clock.uncertainty_ns = 1;
        if (invalid == 6) bad.clock.host_ns = -1;
        if (invalid == 7) bad.clock.counter_hz = 0;
        unsigned char before[sizeof(stream)]; memcpy(before, &stream, sizeof(stream));
        assert(!ssc_imu_begin(&stream, &bad)); assert(!memcmp(before, &stream, sizeof(stream)));
    }
    stream.generation = UINT64_MAX;
    assert(!ssc_imu_begin(&stream, &config));
    assert(!ssc_imu_begin(NULL, &config) && !ssc_imu_begin(&stream, NULL));
    assert(!ssc_imu_report(NULL, 1, 1025, 1, 2, anchor, packed, 16, now, &event));
    assert(!ssc_imu_report(&stream, stream.generation, 1025, 1, 2, anchor, packed, 16, now, NULL));
    ssc_imu_invalidate(NULL);
}

static void corpus(const char *name)
{
    FILE *file = fopen(name, "rb"); assert(file);
    unsigned char header[24], payload[4096]; unsigned count = 0;
    while (1) {
        size_t got = fread(header, 1, sizeof(header), file);
        if (!got) break;
        assert(got == sizeof(header));
        size_t length = le32(header); assert(length <= sizeof(payload));
        assert(fread(payload, 1, length, file) == length);
        struct ssc_imu_vector vector; memset(&vector, 0x5a, sizeof(vector));
        assert(decode(payload, length, &vector) == (le32(header + 4) != 0));
        if (le32(header + 4)) {
            for (unsigned i = 0; i < 3; i++) {
                uint32_t expected = le32(header + 8 + i * 4);
                assert(!memcmp(&expected, &vector.values[i], 4));
            }
            assert(vector.accuracy == le32(header + 20));
        }
        count++;
    }
    assert(!ferror(file) && !fclose(file) && count);
    printf("SSC_IMU_CORPUS_PASS %u\n", count);
}

int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 2);
    payload_tests(); stream_tests();
    if (argc == 2) corpus(argv[1]);
    printf("SSC_IMU_UNIT_PASS %u checks\n", checks);
    return 0;
}
