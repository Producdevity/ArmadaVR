#define _GNU_SOURCE
#include "quest-camera-shutter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static void put(uint8_t *p, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static void refused(const void *p, size_t size, uint32_t bit)
{
    struct quest_camera_shutter out, saved;
    memset(&out, 0xa5, sizeof(out));
    memcpy(&saved, &out, sizeof(out));
    CHECK(!quest_camera_shutter_decode(p, size, bit, &out));
    CHECK(!memcmp(&out, &saved, sizeof(out)));
}
int main(void)
{
    uint8_t packet[213] = {0};
    const uint8_t types[] = {6, 21, 22};
    put(packet + 6, UINT64_C(0xfedcba9876543210), 8);
    put(packet + 24, UINT64_C(0x0123456789abcdef), 8);
    put(packet + 14, UINT32_MAX, 4);
    packet[5] = 0x72; packet[22] = 0x29;
    put(packet + 18, 4500, 2); put(packet + 20, 48, 2);
    for (unsigned t = 0; t < 3; ++t) {
        packet[0] = types[t];
        for (unsigned mask = 1; mask < 64; ++mask) {
            put(packet + 1, mask, 4);
            unsigned cameras[6], count = 0;
            for (unsigned i = 0; i < 6; ++i) if (mask & (1u << i)) cameras[count++] = i;
            for (unsigned extra = 0; extra < count; ++extra) {
                packet[88] = (uint8_t)extra;
                for (unsigned i = 0; i < extra; ++i) {
                    put(packet + 89 + i * 4, 3000 + i, 2);
                    put(packet + 91 + i * 4, 60 + i, 2);
                }
                for (unsigned index = 0; index < count; ++index) {
                    struct quest_camera_shutter out;
                    CHECK(quest_camera_shutter_decode(packet, 89 + extra * 4, 1u << cameras[index], &out));
                    CHECK(out.timestamp_us == UINT64_C(0xfedcba9876543210));
                    CHECK(out.sof_us == UINT64_C(0x0123456789abcdef));
                    CHECK(out.camera_mask == mask && out.frame_counter == UINT32_MAX);
                    CHECK(out.type == types[t] && out.tag == 0x72 && out.auxiliary_tag == 0x29);
                    CHECK(out.exposure_us == (index && index <= extra ? 3000 + index - 1 : 4500));
                    CHECK(out.gain_code == (index && index <= extra ? 60 + index - 1 : 48));
                }
            }
        }
    }
    packet[0] = 22; put(packet + 1, UINT32_MAX, 4); packet[88] = 31;
    for (unsigned i = 0; i < 31; ++i) {
        put(packet + 89 + i * 4, 3000 + i, 2);
        put(packet + 91 + i * 4, 60 + i, 2);
    }
    for (unsigned i = 0; i < 32; ++i) {
        struct quest_camera_shutter out;
        CHECK(quest_camera_shutter_decode(packet, sizeof(packet), UINT32_C(1) << i, &out));
        CHECK(out.exposure_us == (i ? 2999 + i : 4500));
    }
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096);
    uint8_t *region = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(region != MAP_FAILED);
    CHECK(!mprotect(region + page, (size_t)page, PROT_NONE));
    for (size_t n = 0; n < sizeof(packet); ++n) {
        uint8_t *row = region + page - n;
        memcpy(row, packet, n);
        refused(row, n, 1);
    }
    for (unsigned offset = 0; offset < 16; ++offset) {
        uint8_t row[240] = {0};
        memcpy(row + offset, packet, sizeof(packet));
        struct quest_camera_shutter out;
        CHECK(quest_camera_shutter_decode(row + offset, sizeof(packet), 1, &out));
    }
    refused(NULL, sizeof(packet), 1);
    refused(packet, 214, 1);
    refused(packet, sizeof(packet), 0);
    refused(packet, sizeof(packet), 3);
    CHECK(!quest_camera_shutter_decode(packet, sizeof(packet), 1, NULL));
    packet[88] = 32; refused(packet, sizeof(packet), 1);
    packet[88] = 0;
    put(packet + 1, 1, 4);
    refused(packet, 89, 2);
    put(packet + 1, 0, 4);
    refused(packet, 89, 1);
    put(packet + 1, 1, 4);
    for (unsigned type = 0; type < 256; ++type) {
        if (type == 6 || type == 21 || type == 22) continue;
        packet[0] = (uint8_t)type;
        refused(packet, 89, 1);
    }
    CHECK(!munmap(region, (size_t)page * 2));
    printf("QUEST_CAMERA_SHUTTER_PASS checks=%u hardware=0\n", checks);
}
