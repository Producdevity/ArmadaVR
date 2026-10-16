#define _GNU_SOURCE
#include "quest-camera-metadata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
struct vector {
    unsigned sensor;
    const char *hex;
    struct quest_camera_metadata_parameters parameters;
    struct quest_camera_metadata expected;
};
static const struct vector vectors[] = {
    {1, "c02fead873359f7f0b62048800934f39d96474c21a5512b0", {{40, 41274, 38425}, 0}, {8, 194, 18517, 140, {41274, 38425}, 12808, 176, 0}},
    {1, "fa9645a4081537d95de9f174f25f5093ec2c6e1cc337b6d5f942a2d89dd2", {{29, 59932, 45587}, 0}, {10, 87, 8980, 220, {59932, 45587}, 11017, 119, 0}},
    {2, "dafcd372d622a7aba6d870680344083940fc01f65485ae94d2e50c76384ff4035bd7ed95a2edef6df2f65d4be996d57f11f289f5dba2dc5e8d25c8d47944345898a528aac14e57480a23e635d37de06abedf72c61b8c1e95f8d3", {{40, 0, 0}, 246}, {8, 211, 35769, 28044, {14656, 64513}, 53761, 114, 27}},
    {2, "f79cfd75d7e3f8f795ef3bb9ff3a66a900218286403cedfed07757bce1bb462a47be05a7e2f4823c588cb61bee1432e0a9691919eb36c0eb1432c790e77c9701ccc2cc5c9934b9a097224008d3f75a41fc40dbddf189daed3ee5d97f4a9379ec6b8050f4c9167127dc6784eec12c76d1", {{15, 0, 0}, 170}, {10, 209, 19806, 19890, {33344, 15597}, 4874, 113, 103}},
};
static void refused(const void *row, size_t length, unsigned sensor,
                    const struct quest_camera_metadata_parameters *p)
{
    struct quest_camera_metadata output, saved;
    memset(&output, 0xa5, sizeof(output));
    memcpy(&saved, &output, sizeof(saved));
    CHECK(!quest_camera_metadata_decode(row, length, sensor, p, &output));
    CHECK(!memcmp(&output, &saved, sizeof(output)));
}
int main(void)
{
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096);
    uint8_t *region = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(region != MAP_FAILED);
    CHECK(!mprotect(region + page, (size_t)page, PROT_NONE));
    for (size_t v = 0; v < sizeof(vectors) / sizeof(vectors[0]); ++v) {
        const struct vector *entry = &vectors[v];
        size_t length = strlen(entry->hex) / 2;
        uint8_t bytes[128];
        for (size_t i = 0; i < length; ++i) {
            unsigned value;
            CHECK(sscanf(entry->hex + i * 2, "%2x", &value) == 1);
            bytes[i] = (uint8_t)value;
        }
        for (size_t n = 0; n < length; ++n) {
            uint8_t *row = region + page - n;
            memcpy(row, bytes, n);
            refused(row, n, entry->sensor, &entry->parameters);
        }
        for (unsigned alignment = 0; alignment < 16; ++alignment) {
            uint8_t row[160] = {0};
            memcpy(row + alignment, bytes, length);
            struct quest_camera_metadata got;
            CHECK(quest_camera_metadata_decode(row + alignment, length, entry->sensor,
                                                &entry->parameters, &got));
            CHECK(!memcmp(&got, &entry->expected, sizeof(got)));
        }
        uint8_t *row = region + page - length;
        memcpy(row, bytes, length);
        struct quest_camera_metadata got;
        CHECK(quest_camera_metadata_decode(row, length, entry->sensor, &entry->parameters, &got));
        refused(NULL, length, entry->sensor, &entry->parameters);
        refused(row, length, entry->sensor, NULL);
        refused(row, 16385, entry->sensor, &entry->parameters);
        refused(row, length, 0, &entry->parameters);
        refused(row, length, UINT32_MAX, &entry->parameters);
        CHECK(!quest_camera_metadata_decode(row, length, entry->sensor, &entry->parameters, NULL));
        struct quest_camera_metadata_parameters bad = entry->parameters;
        bad.mcu_words[0] = 0;
        refused(row, length, entry->sensor, &bad);
        bad.mcu_words[0] = UINT32_MAX;
        refused(row, length, entry->sensor, &bad);
        bad = entry->parameters;
        bad.mcu_words[1] = UINT32_MAX;
        refused(row, length, entry->sensor, &bad);
        bad = entry->parameters;
        bad.mcu_words[2] = UINT32_MAX;
        refused(row, length, entry->sensor, &bad);
        bad = entry->parameters;
        bad.gain_multiplier = UINT32_MAX;
        refused(row, length, entry->sensor, &bad);
        if (entry->sensor == QUEST_CAMERA_OV7251) {
            bad.gain_multiplier = 65535;
            refused(row, length, entry->sensor, &bad);
            bad.gain_multiplier = 0;
            CHECK(quest_camera_metadata_decode(row, length, entry->sensor, &bad, &got));
            CHECK(got.gain_code == entry->expected.gain_code / entry->parameters.gain_multiplier);
            row[16] = row[20] = 0;
        } else {
            row[5] = row[9] = 0;
        }
        refused(row, length, entry->sensor, &entry->parameters);
    }
    uint8_t row[128] = {0};
    struct quest_camera_metadata_parameters p = {{120, 1, 0}, 0};
    struct quest_camera_metadata out;
    row[5] = 0x35;
    row[3] = 255;
    row[7] = 212;
    CHECK(quest_camera_metadata_decode(row, 24, QUEST_CAMERA_OG01A1B, &p, &out));
    CHECK(out.exposure_us == 65535);
    row[7] = 213;
    refused(row, 24, QUEST_CAMERA_OG01A1B, &p);
    row[3] = row[7] = 0;
    p.mcu_words[2] = 65535;
    refused(row, 24, QUEST_CAMERA_OG01A1B, &p);
    p.mcu_words[2] = 65534;
    CHECK(quest_camera_metadata_decode(row, 24, QUEST_CAMERA_OG01A1B, &p, &out));
    CHECK(out.span_us == 65535);
    CHECK(!munmap(region, (size_t)page * 2));
    printf("QUEST_CAMERA_METADATA_PASS checks=%u hardware=0\n", checks);
}
