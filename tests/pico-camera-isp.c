#include "pico-camera-isp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
static uint32_t get32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static void refused(struct pico_camera_ife_config c)
{
    uint8_t bytes[PICO_CAMERA_IFE_ACQUIRE_SIZE + 1], before[sizeof(bytes)];
    memset(bytes, 0xa5, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes));
    assert(!pico_camera_ife_acquire(&c, bytes + 1, sizeof(bytes) - 1));
    assert(!memcmp(bytes, before, sizeof(bytes)));
    ++checks;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--encode")) {
        unsigned format;
        struct pico_camera_ife_config c;
        while (scanf("%u%u%u%u%u%u%u%u%u%u", &format, &c.bits, &c.phy, &c.cphy,
                     &c.lanes, &c.lane_map, &c.virtual_channel, &c.data_type, &c.width, &c.height) == 10) {
            c.output = format == 7 ? PICO_CAMERA_RAW_PLAIN8 :
                       format == 10 ? PICO_CAMERA_RAW_PLAIN16 : PICO_CAMERA_RAW_MIPI;
            assert(format == 7 || format == 9 || format == 10);
            uint8_t bytes[PICO_CAMERA_IFE_ACQUIRE_SIZE];
            assert(pico_camera_ife_acquire(&c, bytes, sizeof(bytes)));
            printf("%u ", format);
            for (size_t i = 0; i < sizeof(bytes); ++i) printf("%02x", bytes[i]);
            putchar('\n');
        }
        assert(feof(stdin));
        return 0;
    }
    assert(argc == 1);
    struct pico_camera_ife_config c = {.phy = 1, .lanes = 2, .lane_map = 0x3210,
        .bits = 8, .data_type = 0x2a, .width = 640, .height = 400,
        .output = PICO_CAMERA_RAW_PLAIN8};
    uint8_t bytes[PICO_CAMERA_IFE_ACQUIRE_SIZE + 2], before[sizeof(bytes)];
    memset(bytes, 0xa5, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes));
    assert(!pico_camera_ife_acquire(NULL, bytes, sizeof(bytes)));
    assert(!pico_camera_ife_acquire(&c, NULL, sizeof(bytes)));
    for (size_t i = 0; i < PICO_CAMERA_IFE_ACQUIRE_SIZE; ++i) {
        assert(!pico_camera_ife_acquire(&c, bytes, i));
        assert(!memcmp(bytes, before, sizeof(bytes)));
        ++checks;
    }
    for (unsigned bits = 8; bits <= 10; bits += 2)
    for (unsigned fmt = 0; fmt <= PICO_CAMERA_RAW_MIPI; ++fmt)
    for (unsigned phy = 0; phy < 6; ++phy)
    for (unsigned cphy = 0; cphy < 2; ++cphy)
    for (unsigned lanes = 1; lanes <= (cphy ? 3U : 4U); ++lanes)
    for (unsigned vc = 0; vc < 4; ++vc)
    for (unsigned dim = 1; dim <= 4096; dim *= 2) {
        c.bits = bits; c.data_type = bits == 8 ? 0x2a : 0x2b; c.output = fmt;
        c.phy = phy; c.cphy = cphy; c.lanes = lanes; c.virtual_channel = vc;
        c.width = dim; c.height = 4097 - dim;
        if (fmt == PICO_CAMERA_RAW_PLAIN8 && bits == 10) { refused(c); continue; }
        assert(pico_camera_ife_acquire(&c, bytes + 1, sizeof(bytes) - 2));
        assert(bytes[0] == 0xa5 && bytes[sizeof(bytes) - 1] == 0xa5);
        assert(get32(bytes + 1 + 24) == 0x4001 + phy);
        assert(get32(bytes + 1 + 24 + 68) + 1 == c.width);
        assert(get32(bytes + 1 + 176 + 8) == c.width);
        assert(get32(bytes + 1 + 176 + 12) == c.height);
        for (size_t i = 216; i < PICO_CAMERA_IFE_ACQUIRE_SIZE; ++i) assert(bytes[i + 1] == 0);
        ++checks;
    }
    c = (struct pico_camera_ife_config){.phy = 1, .lanes = 2, .lane_map = 0x3210,
        .bits = 8, .data_type = 0x2a, .width = 640, .height = 400,
        .output = PICO_CAMERA_RAW_PLAIN8};
#define BAD(field, value) do { struct pico_camera_ife_config bad = c; bad.field = value; refused(bad); } while (0)
    BAD(phy, 6); BAD(phy, UINT32_MAX); BAD(cphy, 2); BAD(lanes, 0); BAD(lanes, 5);
    BAD(lane_map, 0x10000); BAD(virtual_channel, 4); BAD(data_type, 0x2b);
    BAD(bits, 0); BAD(bits, 12); BAD(width, 0); BAD(width, 4097);
    BAD(height, 0); BAD(height, UINT32_MAX); BAD(output, -1); BAD(output, 3);
    c.cphy = 1; BAD(lanes, 4);
    printf("PICO_CAMERA_ISP_PASS checks=%u\n", checks);
    return 0;
}
