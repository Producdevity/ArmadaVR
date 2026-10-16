#include "quest-camera-sensor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef QUEST_CAMERA_VENDOR_TEST
#include <sys/types.h>
#include <media/cam_sensor.h>
_Static_assert(sizeof(struct cam_cmd_i2c_info) == 8 && sizeof(struct i2c_rdwr_header) == 8, "sensor headers");
_Static_assert(sizeof(struct i2c_random_wr_payload) == 8 && sizeof(struct cam_cmd_unconditional_wait) == 8, "sensor records");
_Static_assert(CAMERA_SENSOR_CMD_TYPE_I2C_INFO == 4 && CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR == 5 && CAMERA_SENSOR_CMD_TYPE_WAIT == 9, "sensor commands");
_Static_assert(CAMERA_SENSOR_WAIT_OP_HW_UCND == 2 && CAMERA_SENSOR_WAIT_OP_SW_UCND == 3, "wait operations");
_Static_assert(sizeof(struct cam_cmd_probe) == 20 && sizeof(struct cam_cmd_power) == 20 && sizeof(struct cam_power_settings) == 12, "probe and power records");
#endif

static void refused(struct quest_camera_sensor_bus bus, struct quest_camera_sensor_write value)
{
    uint8_t out[48], before[48];
    memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
    assert(!quest_camera_sensor_writes_size(&bus, &value, 1));
    assert(!quest_camera_sensor_writes(&bus, &value, 1, out, sizeof(out)));
    assert(!memcmp(out, before, sizeof(out)));
}

int main(int argc, char **argv)
{
    struct quest_camera_sensor_bus bus = {.address_type = 2, .data_type = 1, .frequency = 1};
    if (argc == 2 && !strcmp(argv[1], "--encode")) {
        unsigned slave, count, include;
        while (scanf("%u%u%u%u%u%u", &bus.address_type, &bus.data_type, &bus.frequency, &include, &slave, &count) == 6) {
            assert(include <= 1 && count <= QUEST_CAMERA_SENSOR_MAX_WRITES);
            bus.include_slave = include; bus.slave_address = slave;
            struct quest_camera_sensor_write *writes = calloc(count, sizeof(*writes)); assert(writes);
            for (unsigned i = 0; i < count; ++i)
                assert(scanf("%u%u%u", &writes[i].address, &writes[i].value, &writes[i].delay_us) == 3);
            size_t size = quest_camera_sensor_writes_size(&bus, writes, count); assert(size);
            uint8_t *out = malloc(size); assert(out);
            assert(quest_camera_sensor_writes(&bus, writes, count, out, size) == size);
            for (size_t i = 0; i < size; ++i) printf("%02x", out[i]);
            putchar('\n'); free(out); free(writes);
        }
        assert(feof(stdin)); return 0;
    }
    assert(argc == 1);
    struct quest_camera_sensor_write writes[] = {{0x100, 1, 0}, {0x101, 2, 1999}, {0x102, 3, 2000}};
    uint8_t out[65], before[65];
    memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
    assert(!quest_camera_sensor_writes_size(NULL, writes, 3));
    assert(!quest_camera_sensor_writes_size(&bus, NULL, 3));
    assert(!quest_camera_sensor_writes_size(&bus, writes, 0));
    assert(!quest_camera_sensor_writes_size(&bus, writes, QUEST_CAMERA_SENSOR_MAX_WRITES + 1));
    assert(!quest_camera_sensor_writes(&bus, writes, 3, NULL, 65));
    assert(quest_camera_sensor_writes_size(&bus, writes, 3) == 56);
    for (size_t size = 0; size < 56; ++size) {
        assert(!quest_camera_sensor_writes(&bus, writes, 3, out + 1, size));
        assert(!memcmp(out, before, sizeof(out)));
    }
    assert(quest_camera_sensor_writes(&bus, writes, 3, out + 1, 64) == 56);
    static const uint8_t expected[] = {
        2,0,0,0,1,5,1,2,0,1,0,0,1,0,0,0,1,1,0,0,2,0,0,0,
        207,7,0,0,2,9,0,0,1,0,0,0,1,5,1,2,2,1,0,0,3,0,0,0,
        2,0,0,0,3,9,0,0,
    };
    assert(!memcmp(out + 1, expected, sizeof(expected)));
    assert(out[0] == 0xa5);
    for (size_t i = 57; i < sizeof(out); ++i) assert(out[i] == 0xa5);
    struct quest_camera_sensor_write value = {0x100, 1, 0};
#define BAD_BUS(field, v) do { struct quest_camera_sensor_bus b = bus; b.field = v; refused(b, value); } while (0)
#define BAD_WRITE(field, v) do { struct quest_camera_sensor_write w = value; w.field = v; refused(bus, w); } while (0)
    BAD_BUS(address_type, 0); BAD_BUS(address_type, 5); BAD_BUS(data_type, 0); BAD_BUS(data_type, 5);
    BAD_BUS(frequency, 4); BAD_WRITE(address, 65536); BAD_WRITE(value, 256);
    BAD_WRITE(delay_us, 2001); BAD_WRITE(delay_us, 32768000); BAD_WRITE(delay_us, UINT32_MAX);
    bus.include_slave = true; bus.slave_address = 0xea;
    assert(quest_camera_sensor_writes_size(&bus, writes, 3) == 64);
    assert(quest_camera_sensor_writes(&bus, writes, 3, out + 1, 64) == 64);
    assert(out[1] == 0xea && out[5] == 1 && out[6] == 4 && out[8] == 0);
    BAD_BUS(slave_address, 0); BAD_BUS(slave_address, 255); BAD_BUS(slave_address, 0xeb); BAD_BUS(slave_address, 256);
    unsigned cases = 0;
    for (unsigned at = 1; at <= 4; ++at)
    for (unsigned dt = 1; dt <= 4; ++dt)
    for (unsigned frequency = 0; frequency < 4; ++frequency) {
        bus.address_type = at; bus.data_type = dt; bus.frequency = frequency;
        value.address = (uint32_t)(((uint64_t)1 << (at * 8)) - 1);
        value.value = (uint32_t)(((uint64_t)1 << (dt * 8)) - 1);
        value.delay_us = 32767000;
        assert(quest_camera_sensor_writes(&bus, &value, 1, out + 1, 64) == 32);
        ++cases;
    }
    struct quest_camera_sensor_write *many = calloc(QUEST_CAMERA_SENSOR_MAX_WRITES, sizeof(*many)); assert(many);
    bus.include_slave = false;
    size_t size = quest_camera_sensor_writes_size(&bus, many, QUEST_CAMERA_SENSOR_MAX_WRITES);
    assert(size == 8 + QUEST_CAMERA_SENSOR_MAX_WRITES * 8);
    uint8_t *large = malloc(size); assert(large);
    assert(quest_camera_sensor_writes(&bus, many, QUEST_CAMERA_SENSOR_MAX_WRITES, large, size) == size);
    free(large); free(many);
    printf("QUEST_CAMERA_SENSOR_PASS matrix=%u\n", cases); return 0;
}
