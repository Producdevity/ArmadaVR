#include "pico-controller.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void unhex(const char *s, unsigned char data[100])
{
    assert(strlen(s) == 200);
    for (unsigned i = 0; i < 100; i++) { unsigned value; assert(sscanf(s + i*2, "%2x", &value) == 1); data[i] = value; }
}
static void checksum(unsigned char data[100])
{
    unsigned crc = 0;
    for (unsigned i = 0; i < 99; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (crc & 1 ? 0x8c : 0);
    }
    data[99] = crc;
}
static unsigned checks;
static void rejected(const void *data, size_t size, unsigned mcu, unsigned radio, unsigned connected)
{
    struct pico_controller_imu_batch before, out;
    memset(&before, 0x5a, sizeof(before)); memcpy(&out, &before, sizeof(out));
    assert(!pico_controller_decode_imu(data, size, mcu, radio, connected, &out));
    assert(!memcmp(&out, &before, sizeof(out))); ++checks;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--decode")) {
        unsigned id, mcu, radio, connected; char text[201]; unsigned char raw[100];
        while (scanf("%u%u%u%u %200s", &id, &mcu, &radio, &connected, text) == 5) {
            unhex(text, raw); struct pico_controller_imu_batch batch;
            assert(pico_controller_decode_imu(raw, sizeof(raw), mcu, radio, connected, &batch));
            printf("%u %u %" PRIu64, id, batch.count, batch.station_time);
            for (unsigned i = 0; i < batch.count; i++) {
                const struct pico_controller_imu_sample *p = &batch.samples[i];
                printf(" %u %" PRIu64 " %u", p->hand, p->timestamp_us, p->sequence);
                for (unsigned j = 0; j < 3; j++) printf(" %d", p->gyroscope[j]);
                for (unsigned j = 0; j < 3; j++) printf(" %d", p->acceleration[j]);
                printf(" %d", p->temperature);
            }
            putchar('\n');
        }
        return 0;
    }
    assert(argc == 1);
    unsigned char raw[100];
    unhex("a507d4aacfffd7fa277fab19bf5fb3274129ee75a68f4ac868e6398f950000004b52b46f9a9f6d8103c1c106d5b03ad6f8c5b9aa00000051ee3e2e7e35256d238645f938496de54465eba9e807c9bde0bd09ad672ed82effad6f71c4b67d19f69fb8061b", raw);
    const int16_t gyro[2][3] = {{6571,24511,10163},{-19374,-26001,28063}};
    const int16_t accel[2][3] = {{10561,30190,-28762},{897,-15935,-11002}};
    const int16_t temperature[] = {-14262,15024};
    const uint8_t counters[] = {75,81};
    const uint64_t times[] = {642353063528,733261199574};
    const unsigned slots[2][2][3] = {{{3,3,3},{2,2,2}},{{3,1,3},{2,1,3}}};
    for (unsigned mcu = 1; mcu <= 2; mcu++)
    for (unsigned radio = 1; radio <= 2; radio++)
    for (unsigned connected = 1; connected <= 3; connected++)
    for (unsigned hand = 1; hand <= 2; hand++) {
        raw[7] = (raw[7] & ~0x40) | (hand == 1 ? 0x40 : 0); checksum(raw);
        struct pico_controller_imu_batch out;
        assert(pico_controller_decode_imu(raw, 100, mcu, radio, connected, &out));
        unsigned count = 0;
        for (unsigned slot = 0; slot < 2; slot++) if (slots[mcu-1][radio-1][connected-1] & (1u << slot)) {
            const struct pico_controller_imu_sample *p = &out.samples[count++];
            assert(p->hand == (mcu == 2 ? 2-slot : hand));
            assert(p->timestamp_us == times[slot] && p->sequence == counters[slot]);
            assert(!memcmp(p->gyroscope, gyro[slot], sizeof(p->gyroscope)));
            assert(!memcmp(p->acceleration, accel[slot], sizeof(p->acceleration)));
            assert(p->temperature == temperature[slot]);
        }
        assert(out.count == count); ++checks;
    }
    for (unsigned byte = 0; byte < 100; byte++) for (unsigned bit = 0; bit < 8; bit++) {
        raw[byte] ^= 1u << bit; rejected(raw, 100, 1, 1, 3); raw[byte] ^= 1u << bit;
    }
    for (unsigned length = 0; length < 100; length++) rejected(raw, length, 1, 1, 3);
    rejected(raw, 101, 1, 1, 3); rejected(NULL, 100, 1, 1, 3);
    rejected(raw, 100, 0, 1, 3); rejected(raw, 100, 3, 1, 3);
    rejected(raw, 100, 1, 0, 3); rejected(raw, 100, 1, 3, 3);
    rejected(raw, 100, 1, 1, 0); rejected(raw, 100, 1, 1, 4);
    assert(!pico_controller_decode_imu(raw, 100, 1, 1, 3, NULL));
    raw[0] = 0xbb; checksum(raw); rejected(raw, 100, 1, 1, 3); raw[0] = 0xa5;
    for (unsigned type = 0; type < 8; type += 2) { raw[1] = type; checksum(raw); rejected(raw, 100, 2, 2, 3); }
    raw[1] = 1; memset(raw+24, 0, 8); memset(raw+47, 0, 8); checksum(raw);
    struct pico_controller_imu_batch out;
    memset(&out, 0x5a, sizeof(out)); assert(pico_controller_decode_imu(raw, 100, 1, 1, 3, &out) && out.count == 0);
    memset(raw+24, 0xff, 8); checksum(raw);
    assert(pico_controller_decode_imu(raw, 100, 1, 1, 3, &out) && out.count == 1 && out.samples[0].timestamp_us == UINT64_MAX);
    unsigned char unaligned[101]; memcpy(unaligned+1,raw,100);
    assert(pico_controller_decode_imu(unaligned+1, 100, 1, 1, 3, &out) && out.count == 1);
    unsigned char payload[104] = {0x0a,100}; memcpy(payload+2,raw,100); payload[102]=0x10; payload[103]=3;
    unsigned accuracy = 99;
    assert(pico_controller_decode_imu_ssc(payload, sizeof(payload), 1, 1, 3, &out, &accuracy));
    assert(accuracy == 3 && out.count == 1 && out.samples[0].timestamp_us == UINT64_MAX);
    struct pico_controller_imu_batch before = out;
    for (unsigned length = 0; length < sizeof(payload); length++) {
        accuracy = 99;
        assert(!pico_controller_decode_imu_ssc(payload, length, 1, 1, 3, &out, &accuracy));
        assert(accuracy == 99 && !memcmp(&out,&before,sizeof(out))); ++checks;
    }
    payload[103]=4; accuracy=99;
    assert(!pico_controller_decode_imu_ssc(payload, sizeof(payload), 1, 1, 3, &out, &accuracy));
    assert(accuracy == 99 && !memcmp(&out,&before,sizeof(out)));
    payload[103]=3;
    assert(!pico_controller_decode_imu_ssc(payload, sizeof(payload), 1, 1, 3, NULL, &accuracy));
    assert(!pico_controller_decode_imu_ssc(payload, sizeof(payload), 1, 1, 3, &out, NULL));
    printf("PICO_CONTROLLER_IMU_PASS checks=%u\n", checks);
}
