#include "ssc-calibration.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error Assertions must be enabled
#endif

static unsigned checks;
static uint32_t seed = 0x53534343;

static uint32_t random_word(void)
{
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static const unsigned char bias[] = {
    0x0a, 12, 0, 0, 0x80, 0x3f, 0, 0, 0, 0xc0, 0, 0, 0x60, 0x40, 0x20, 3,
};

static bool decode(const void *p, size_t length, struct ssc_calibration_value *value)
{
    unsigned char saved[sizeof(*value)]; memcpy(saved, value, sizeof(saved));
    bool valid = ssc_calibration_decode(p, length, value);
    if (!valid) assert(!memcmp(saved, value, sizeof(saved)));
    checks++;
    return valid;
}

static void wire_tests(void)
{
    struct ssc_calibration_value value;
    memset(&value, 0xa5, sizeof(value));
    assert(decode(bias, sizeof(bias), &value));
    assert(value.bias[0] == 1 && value.bias[1] == -2 && value.bias[2] == 3.5f);
    assert(value.accuracy == 3 && !value.has_scale && !value.has_matrix && !value.has_cal_id);
    for (size_t i = 0; i < sizeof(bias); i++) assert(!decode(bias, i, &value));
    unsigned char payload[4200];
    memcpy(payload + 1, bias, sizeof(bias));
    assert(decode(payload + 1, sizeof(bias), &value));
    for (unsigned accuracy = 0; accuracy < 5; accuracy++) {
        memcpy(payload, bias, sizeof(bias)); payload[15] = accuracy;
        assert(decode(payload, 16, &value) == (accuracy < 4));
    }
    for (unsigned field = 1; field <= 3; field++) {
        for (unsigned wire = 0; wire < 8; wire++) {
            memcpy(payload, bias, sizeof(bias)); payload[16] = field * 8 + wire;
            memset(payload + 17, 0, 80);
            if (wire != 2) assert(!decode(payload, 21, &value));
        }
    }
    const unsigned char invalid[][12] = {
        {0}, {0x1b, 0x1c}, {0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2},
        {0x2a, 0}, {0x20, 4}, {0x12, 1, 0}, {0x1a, 1, 0},
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        memcpy(payload, bias, sizeof(bias)); memcpy(payload + 16, invalid[i], 12);
        assert(!decode(payload, 28, &value));
    }
    assert(!decode(NULL, 0, &value)); assert(!decode(payload, 4097, &value));
    assert(!ssc_calibration_decode(bias, 16, NULL));
    for (unsigned i = 0; i < 100000; i++) {
        size_t length = random_word() % 96;
        for (size_t j = 0; j < length; j++) payload[j] = random_word();
        if (i & 1) {
            memcpy(payload, bias, 16); length = 16;
            payload[random_word() % 16] ^= random_word();
        }
        (void)decode(payload, length, &value);
    }
}

static bool report(struct ssc_calibration *c, unsigned sensor, uint64_t ticks, unsigned accuracy)
{
    unsigned char payload[sizeof(bias)]; memcpy(payload, bias, sizeof(bias)); payload[15] = accuracy;
    struct ssc_calibration before = *c;
    bool valid = ssc_calibration_report(c, c->generation, 1022, c->config.suid_high[sensor],
        c->config.suid_low[sensor], ticks, payload, sizeof(payload), 1000000);
    if (!valid) assert(!memcmp(c, &before, sizeof(before)));
    checks++;
    return valid;
}

static bool correct(struct ssc_calibration *c, unsigned sensor, uint64_t ticks,
                    struct ssc_bias_sample *output)
{
    struct ssc_imu_event event = {.generation = c->generation, .sensor = sensor,
        .ticks = ticks, .vector = {.values = {4, 5, 6}, .accuracy = 3}};
    assert(ssc_clock_map(&c->config.clock, ticks, 1000000, 1000000, &event.time));
    unsigned char before[sizeof(*output)]; memcpy(before, output, sizeof(before));
    struct ssc_calibration saved = *c;
    bool valid = ssc_calibration_correct(c, &event, 1000000, output);
    assert(!memcmp(c, &saved, sizeof(saved)));
    if (!valid) assert(!memcmp(output, before, sizeof(before)));
    else {
        assert(output->corrected[0] == 3 && output->corrected[1] == 7 && output->corrected[2] == 2.5f);
        assert(!memcmp(&output->raw, &event, sizeof(event)));
        assert(output->calibration.ticks <= ticks);
    }
    checks++;
    return valid;
}

static void history_tests(void)
{
    struct ssc_calibration c = {0};
    struct ssc_calibration_config cfg = {.suid_high = {1, 2}, .suid_low = {3, 4},
        .max_report_age_ns = 1000000, .max_bias_age_ns = 1000};
    assert(ssc_clock_init(&cfg.clock, 0, 19200000, 100, 100, 0));
    struct ssc_bias_sample output; memset(&output, 0xa5, sizeof(output));
    assert(!ssc_calibration_begin(&c, &cfg, 0));
    for (uint64_t generation = 1; generation <= 1000; generation++) {
        assert(ssc_calibration_begin(&c, &cfg, generation));
        assert(!correct(&c, 0, 100, &output));
        assert(report(&c, 0, 100, 3));
        assert(!report(&c, 0, 99, 3)); assert(!report(&c, 0, 100, 2));
        assert(!correct(&c, 0, 99, &output)); assert(!correct(&c, 1, 100, &output));
        assert(correct(&c, 0, 100, &output) && output.accuracy == 3);
        assert(correct(&c, 0, 119, &output)); assert(!correct(&c, 0, 120, &output));
        assert(report(&c, 0, 110, 0));
        assert(correct(&c, 0, 109, &output));
        assert(!correct(&c, 0, 110, &output));
        assert(report(&c, 0, 120, 2)); assert(report(&c, 1, 90, 1));
        assert(!correct(&c, 0, 119, &output));
        assert(correct(&c, 0, 120, &output) && output.accuracy == 2);
        assert(correct(&c, 1, 100, &output) && output.accuracy == 1);
        for (unsigned tick = 121; tick < 221; tick++) assert(report(&c, 0, tick, 3));
        assert(c.count[0] == SSC_CALIBRATION_HISTORY);
        assert(!correct(&c, 0, 188, &output));
        assert(correct(&c, 0, 189, &output));
        struct ssc_calibration saved = c;
        assert(!ssc_calibration_begin(&c, &cfg, generation));
        assert(!ssc_calibration_report(&c, generation - 1, 1022, 1, 3, 221, bias, 16, 1000000));
        assert(!ssc_calibration_report(&c, generation, 1025, 1, 3, 221, bias, 16, 1000000));
        assert(!ssc_calibration_report(&c, generation, 1022, 1, 4, 221, bias, 16, 1000000));
        assert(!ssc_calibration_report(&c, generation, 1022, 1, 3, 221, bias, 16, 999999));
        assert(!ssc_calibration_report(&c, generation, 1022, 1, 3, UINT64_MAX, bias, 16, 1000000));
        assert(!memcmp(&c, &saved, sizeof(c)));
        struct ssc_imu_event sample = output.raw;
        sample.generation = generation - 1;
        assert(!ssc_calibration_correct(&c, &sample, 1000000, &output));
        sample.generation = generation; sample.sensor = -1;
        assert(!ssc_calibration_correct(&c, &sample, 1000000, &output));
        sample.sensor = 0; sample.time.timestamp_ns++;
        assert(!ssc_calibration_correct(&c, &sample, 1000000, &output));
        ssc_calibration_invalidate(&c);
        assert(!report(&c, 0, 221, 3)); assert(!correct(&c, 0, 220, &output));
    }
    assert(ssc_calibration_begin(&c, &cfg, UINT64_MAX));
    assert(!ssc_calibration_begin(&c, &cfg, 1));
    assert(report(&c, 0, 100, 3));
    assert(correct(&c, 0, 100, &output));
    struct ssc_imu_event sample = output.raw;
    for (unsigned accuracy = 0; accuracy <= 4; accuracy++) {
        sample.vector.accuracy = accuracy;
        assert(ssc_calibration_correct(&c, &sample, 1000000, &output) == (accuracy > 0 && accuracy <= 3));
        if (accuracy > 0 && accuracy <= 3) assert(output.accuracy == accuracy);
    }
    sample.vector.accuracy = 3; sample.vector.values[0] = INFINITY;
    assert(!ssc_calibration_correct(&c, &sample, 1000000, &output));
    sample.vector.values[0] = FLT_MAX; c.entries[0][0].value.bias[0] = -FLT_MAX;
    assert(!ssc_calibration_correct(&c, &sample, 1000000, &output));
    ssc_calibration_invalidate(NULL);
}

static void corpus(const char *path)
{
    FILE *file = fopen(path, "rb"); assert(file);
    unsigned cases = 0;
    unsigned char header[80], payload[4096];
    size_t count;
    while ((count = fread(header, 1, sizeof(header), file)) != 0) {
        assert(count == sizeof(header));
        uint32_t length = le32(header), valid = le32(header + 4);
        assert(length <= sizeof(payload) && valid <= 1);
        assert(fread(payload, 1, length, file) == length);
        struct ssc_calibration_value value; memset(&value, 0xa5, sizeof(value));
        assert(decode(payload, length, &value) == (bool)valid);
        if (valid) {
            assert(!memcmp(value.bias, header + 8, 12));
            assert(!memcmp(value.scale, header + 20, 12));
            assert(!memcmp(value.matrix, header + 32, 36));
            assert(value.accuracy == le32(header + 68));
            assert(value.cal_id == le32(header + 72));
            unsigned flags = value.has_scale | value.has_matrix << 1 | value.has_cal_id << 2;
            assert(flags == le32(header + 76));
        }
        cases++;
    }
    assert(!ferror(file) && cases > 1000); assert(fclose(file) == 0);
    printf("SSC_CALIBRATION_CORPUS_PASS %u\n", cases);
}

int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 2);
    wire_tests(); history_tests();
    if (argc == 2) corpus(argv[1]);
    printf("SSC_CALIBRATION_UNIT_PASS %u\n", checks);
    return 0;
}
