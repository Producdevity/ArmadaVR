#include "pico-camera-isp.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;

static void refused(struct pico_camera_csiphy_config c)
{
    uint8_t bytes[PICO_CAMERA_CSIPHY_SIZE + 2], before[sizeof(bytes)];
    memset(bytes, 0xa5, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes));
    assert(!pico_camera_csiphy_configure(&c, bytes + 1, sizeof(bytes) - 2));
    assert(!memcmp(bytes, before, sizeof(bytes)));
    ++checks;
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--encode")) {
        struct pico_camera_csiphy_config c;
        while (scanf("%u%u%u%u%u%" SCNu64 "%u", &c.cphy, &c.combo, &c.lanes, &c.lane_map,
                     &c.settle_count, &c.data_rate, &c.mipi_flags) == 7) {
            uint8_t bytes[PICO_CAMERA_CSIPHY_SIZE];
            assert(pico_camera_csiphy_configure(&c, bytes, sizeof(bytes)));
            for (size_t i = 0; i < sizeof(bytes); ++i) printf("%02x", bytes[i]);
            putchar('\n');
        }
        assert(feof(stdin));
        return 0;
    }
    assert(argc == 1);
    struct pico_camera_csiphy_config c = {.lanes = 2, .lane_map = 0x3210,
        .settle_count = 14, .data_rate = 396000000};
    uint8_t bytes[PICO_CAMERA_CSIPHY_SIZE + 2], before[sizeof(bytes)];
    memset(bytes, 0xa5, sizeof(bytes)); memcpy(before, bytes, sizeof(bytes));
    assert(!pico_camera_csiphy_configure(NULL, bytes, sizeof(bytes)));
    assert(!pico_camera_csiphy_configure(&c, NULL, sizeof(bytes)));
    for (size_t i = 0; i < PICO_CAMERA_CSIPHY_SIZE; ++i) {
        assert(!pico_camera_csiphy_configure(&c, bytes, i));
        assert(!memcmp(bytes, before, sizeof(bytes)));
        ++checks;
    }
    static const uint8_t stock[] = {
        7,0,16,50,0,0,2,0,0,156,228,166,0,0,0,0,
        0,123,154,23,0,0,0,0,0,0,0,0,0,0,0,0,
    };
    assert(pico_camera_csiphy_configure(&c, bytes + 1, sizeof(bytes) - 2));
    assert(!memcmp(bytes + 1, stock, sizeof(stock)));
    for (unsigned cphy = 0; cphy < 2; ++cphy)
    for (unsigned combo = 0; combo < 2; ++combo)
    for (unsigned lanes = 1; lanes <= (cphy ? 3U : combo ? 2U : 4U); ++lanes)
    for (unsigned count = 0; count <= 65535; count += 257) {
        c.cphy = cphy; c.combo = combo; c.lanes = lanes;
        c.settle_count = count; c.data_rate = UINT64_MAX - count;
        c.mipi_flags = UINT32_MAX - count;
        assert(pico_camera_csiphy_configure(&c, bytes + 1, sizeof(bytes) - 2));
        assert(bytes[0] == 0xa5 && bytes[sizeof(bytes) - 1] == 0xa5);
        assert(bytes[1 + 7] == 0 && get64(bytes + 1 + 8) == (uint64_t)count * 200000000);
        assert(get64(bytes + 1 + 16) == c.data_rate);
        for (unsigned i = 28; i < PICO_CAMERA_CSIPHY_SIZE; ++i) assert(bytes[1 + i] == 0);
        ++checks;
    }
    c = (struct pico_camera_csiphy_config){.lanes = 2, .lane_map = 0x3210,
        .settle_count = 14, .data_rate = 396000000};
#define BAD(field, value) do { struct pico_camera_csiphy_config bad = c; bad.field = value; refused(bad); } while (0)
    BAD(cphy, 2); BAD(combo, 2); BAD(lanes, 0); BAD(lanes, 5); BAD(lanes, UINT32_MAX);
    BAD(lane_map, 0x10000); BAD(lane_map, 0x00); BAD(lane_map, 0x11); BAD(lane_map, 0x40);
    BAD(settle_count, 65536); BAD(settle_count, UINT32_MAX); BAD(data_rate, 0);
    c.cphy = 1; BAD(lanes, 4); BAD(lane_map, 0x30);
    c.cphy = 0; c.combo = 1; BAD(lanes, 3); BAD(lane_map, 0x30);
    printf("PICO_CAMERA_CSIPHY_PASS checks=%u\n", checks);
    return 0;
}
