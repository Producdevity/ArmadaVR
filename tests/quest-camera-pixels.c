#define _GNU_SOURCE
#include "quest-camera-pixels.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static const uint16_t values[8] = {0, 1, 12, 13, 63, 64, 65, 1023};
static const uint8_t expected[3][8] = {
    {0, 0, 3, 3, 15, 16, 16, 255},
    {2, 2, 11, 12, 53, 51, 51, 255},
    {0, 0, 0, 1, 43, 44, 44, 255},
};
static void packed(uint8_t *row)
{
    for (unsigned group = 0; group < 2; ++group) {
        row[5 * group + 4] = 0;
        for (unsigned i = 0; i < 4; ++i) {
            unsigned value = values[4 * group + i];
            row[5 * group + i] = (uint8_t)(value >> 2);
            row[5 * group + 4] |= (value & 3u) << (2 * i);
        }
    }
}
static void refused(const void *source, size_t length, const struct quest_camera_pixel_layout *layout,
                    size_t output_length)
{
    uint8_t output[32], saved[32];
    memset(output, 0xa5, sizeof(output)); memcpy(saved, output, sizeof(saved));
    CHECK(!quest_camera_pixels_mono8(source, length, layout, output, output_length));
    CHECK(!memcmp(output, saved, sizeof(output)));
}
int main(void)
{
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096);
    uint8_t *region = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(region != MAP_FAILED);
    CHECK(!mprotect(region + page, (size_t)page, PROT_NONE));
    for (unsigned curve = 0; curve < 3; ++curve) {
        for (unsigned first = 0; first < 2; ++first) {
            for (unsigned padding = 0; padding < 16; ++padding) {
                struct quest_camera_pixel_layout l = {8, 2, first, 10 + padding, QUEST_CAMERA_MIPI_RAW10, curve};
                size_t required = (first + 1) * l.stride + 10;
                uint8_t source[96]; memset(source, 0xfd, sizeof(source));
                packed(source + first * l.stride); packed(source + (first + 1) * l.stride);
                for (size_t n = 0; n <= required; ++n) {
                    uint8_t *in = region + page - n;
                    memcpy(in, source, n);
                    if (n < required) refused(in, n, &l, 16);
                    else {
                        uint8_t output[17]; memset(output, 0xa5, sizeof(output));
                        CHECK(quest_camera_pixels_mono8(in, n, &l, output, 16));
                        CHECK(!memcmp(output, expected[curve], 8));
                        CHECK(!memcmp(output + 8, expected[curve], 8));
                        CHECK(output[16] == 0xa5);
                    }
                }
            }
        }
    }
    uint8_t source[64], saved[64], output[8];
    for (unsigned i = 0; i < sizeof(source); ++i) source[i] = (uint8_t)i;
    struct quest_camera_pixel_layout l = {4, 2, 1, 7, QUEST_CAMERA_MONO8, QUEST_CAMERA_CURVE_COPY};
    CHECK(quest_camera_pixels_mono8(source, sizeof(source), &l, output, sizeof(output)));
    CHECK(!memcmp(output, source + 7, 4));
    CHECK(!memcmp(output + 4, source + 14, 4));
    refused(NULL, sizeof(source), &l, 8);
    refused(source, sizeof(source), NULL, 8);
    CHECK(!quest_camera_pixels_mono8(source, sizeof(source), &l, NULL, 8));
    for (unsigned n = 0; n < 16; ++n) if (n != 8) refused(source, sizeof(source), &l, n);
    memcpy(saved, source, sizeof(saved));
    CHECK(!quest_camera_pixels_mono8(source, sizeof(source), &l, source + 8, 8));
    CHECK(!quest_camera_pixels_mono8(source + 8, 32, &l, source + 4, 8));
    CHECK(!memcmp(source, saved, sizeof(saved)));
    refused((void *)(UINTPTR_MAX - 16), 64, &l, 8);
    CHECK(!quest_camera_pixels_mono8(source, sizeof(source), &l, (void *)(UINTPTR_MAX - 3), 8));
    for (unsigned field = 0; field < 12; ++field) {
        struct quest_camera_pixel_layout bad = l;
        switch (field) {
        case 0: bad.width = 0; break;
        case 1: bad.width = 4097; break;
        case 2: bad.height = 0; break;
        case 3: bad.height = 4097; break;
        case 4: bad.first_row = 2; break;
        case 5: bad.stride = 0; break;
        case 6: bad.stride = 3; break;
        case 7: bad.stride = 65537; break;
        case 8: bad.format = 0; break;
        case 9: bad.curve = 3; break;
        case 10: bad.curve = QUEST_CAMERA_CURVE_POINT5; break;
        case 11: bad.format = QUEST_CAMERA_MIPI_RAW10; bad.width = 3; break;
        }
        refused(source, sizeof(source), &bad, 8);
    }
    refused(source, SIZE_MAX, &l, 8);
    CHECK(!munmap(region, (size_t)page * 2));
    printf("QUEST_CAMERA_PIXELS_PASS checks=%u hardware=0\n", checks);
}
