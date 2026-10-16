#include "quest-camera-sensor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--encode")) {
        unsigned up, count;
        while (scanf("%u%u", &up, &count) == 2) {
            assert(up <= 1 && count <= QUEST_CAMERA_SENSOR_MAX_POWER_STEPS);
            struct quest_camera_sensor_power_step steps[QUEST_CAMERA_SENSOR_MAX_POWER_STEPS];
            for (unsigned i = 0; i < count; ++i)
                assert(scanf("%u%u%u", &steps[i].sequence_type, &steps[i].value, &steps[i].delay_ms) == 3);
            size_t size = quest_camera_sensor_power_size(steps, count); assert(size);
            uint8_t *out = malloc(size); assert(out);
            assert(quest_camera_sensor_power(up, steps, count, out, size) == size);
            for (size_t i = 0; i < size; ++i) printf("%02x", out[i]);
            putchar('\n'); free(out);
        }
        assert(feof(stdin)); return 0;
    }
    assert(argc == 1);
    struct quest_camera_sensor_power_step steps[] = {{8, 0, 0}, {0, 24000000, 1}, {11, 1, 32767}};
    uint8_t out[70], before[70];
    memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
    assert(!quest_camera_sensor_power_size(NULL, 1));
    assert(!quest_camera_sensor_power_size(steps, 0));
    assert(!quest_camera_sensor_power_size(steps, QUEST_CAMERA_SENSOR_MAX_POWER_STEPS + 1));
    assert(!quest_camera_sensor_power(true, steps, 3, NULL, sizeof(out)));
    assert(quest_camera_sensor_power_size(steps, 3) == 68);
    for (size_t size = 0; size < 68; ++size) {
        assert(!quest_camera_sensor_power(true, steps, 3, out + 1, size));
        assert(!memcmp(out, before, sizeof(out)));
    }
    static const uint8_t expected[] = {
        2,0,0,0,0,2,0,0, 8,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,54,110,1,0,0,0,0, 1,0,0,0,3,9,0,0,
        1,0,0,0,0,2,0,0, 11,0,0,0,1,0,0,0,0,0,0,0,
        255,127,0,0,3,9,0,0,
    };
    assert(quest_camera_sensor_power(true, steps, 3, out + 1, 68) == 68);
    assert(!memcmp(out + 1, expected, sizeof(expected)) && out[0] == 0xa5 && out[69] == 0xa5);
    assert(quest_camera_sensor_power(false, steps, 3, out + 1, 68) == 68);
    assert(out[6] == 3 && out[46] == 3);
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        struct quest_camera_sensor_power_step bad = {13, 0, 0};
        if (invalid) { bad.sequence_type = 0; bad.delay_ms = invalid == 1 ? 32768 : UINT32_MAX; }
        memset(out, 0xa5, sizeof(out));
        assert(!quest_camera_sensor_power_size(&bad, 1));
        assert(!quest_camera_sensor_power(true, &bad, 1, out, sizeof(out)));
        assert(!memcmp(out, before, sizeof(out)));
    }
    struct quest_camera_sensor_power_step many[QUEST_CAMERA_SENSOR_MAX_POWER_STEPS] = {{0}};
    assert(quest_camera_sensor_power_size(many, 11) == 140);
    assert(quest_camera_sensor_power_size(many, 12) == 152);
    many[10].delay_ms = 1;
    assert(quest_camera_sensor_power_size(many, 12) == 168);
    uint8_t full[152];
    many[10].delay_ms = 0;
    for (unsigned i = 0; i < 12; ++i) many[i].sequence_type = 12;
    assert(quest_camera_sensor_power(true, many, 12, full, sizeof(full)) == sizeof(full));
    assert(full[0] == 12 && full[5] == 2);
    for (unsigned i = 0; i < 12; ++i) assert(full[8 + 12 * i] == 12);
    unsigned cases = 0;
    for (unsigned type = 0; type < 13; ++type)
    for (unsigned direction = 0; direction < 2; ++direction)
    for (unsigned delay = 0; delay < 2; ++delay) {
        struct quest_camera_sensor_power_step step = {type, UINT32_MAX, delay ? 32767 : 0};
        size_t size = quest_camera_sensor_power_size(&step, 1);
        assert(size == (delay ? 28 : 20));
        assert(quest_camera_sensor_power(direction, &step, 1, out + 1, 68) == size);
        ++cases;
    }
    printf("QUEST_CAMERA_POWER_PASS matrix=%u\n", cases); return 0;
}
