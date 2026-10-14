#include "pico-camera-sensor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--encode")) {
        struct pico_camera_sensor_bus bus = {0};
        struct pico_camera_sensor_probe probe = {0};
        unsigned char out[28];
        bus.include_slave = true;
        while (scanf("%u%u%u%u%u%u%u%u%u", &bus.address_type, &bus.data_type,
                      &bus.frequency, &bus.slave_address, &probe.register_address,
                      &probe.expected_id, &probe.id_mask, &probe.camera_id, &probe.pipeline_delay) == 9) {
            assert(pico_camera_sensor_probe(&bus, &probe, out, sizeof(out)) == sizeof(out));
            for (unsigned i = 0; i < sizeof(out); ++i) printf("%02x", out[i]);
            putchar('\n');
        }
        return 0;
    }
    struct pico_camera_sensor_bus bus = {2, 1, 1, true, 0xe8};
    struct pico_camera_sensor_probe probe = {0x300a, 0x7750, UINT32_MAX, 5, 2};
    const unsigned char expected[28] = {
        0xe8,0,0,0,1,4,0,0,1,2,3,1,0x0a,0x30,0,0,0x50,0x77,0,0,
        0xff,0xff,0xff,0xff,5,0,2,0};
    unsigned char out[30];memset(out, 0xa5, sizeof(out));
    assert(pico_camera_sensor_probe(&bus, &probe, out + 1, 28) == 28);
    assert(!memcmp(out + 1, expected, 28) && out[0] == 0xa5 && out[29] == 0xa5);
    unsigned checks = 0;
    for (unsigned i = 0; i < 13; ++i) {
        struct pico_camera_sensor_bus b = bus;
        struct pico_camera_sensor_probe p = probe;
        switch (i) {
        case 0: b.address_type = 0; break;
        case 1: b.address_type = 5; break;
        case 2: b.data_type = 0; break;
        case 3: b.data_type = 5; break;
        case 4: b.frequency = 4; break;
        case 5: b.slave_address = 0; break;
        case 6: b.slave_address = 0xe9; break;
        case 7: b.slave_address = 256; break;
        case 8: b.include_slave = false; break;
        case 9: p.camera_id = 65536; break;
        case 10: p.pipeline_delay = 4; break;
        case 11: p.register_address = 65536; break;
        default: break;
        }
        memset(out, 0xa5, sizeof(out));
        assert(!pico_camera_sensor_probe(&b, &p, out, i == 12 ? 27 : sizeof(out)));
        for (unsigned j = 0; j < sizeof(out); ++j) assert(out[j] == 0xa5);
        ++checks;
    }
    assert(!pico_camera_sensor_probe(NULL, &probe, out, sizeof(out)));
    assert(!pico_camera_sensor_probe(&bus, NULL, out, sizeof(out)));
    assert(!pico_camera_sensor_probe(&bus, &probe, NULL, sizeof(out)));
    bus.address_type = 4; probe.register_address = UINT32_MAX;
    probe.camera_id = UINT16_MAX; probe.pipeline_delay = 3;
    assert(pico_camera_sensor_probe(&bus, &probe, out, sizeof(out)) == 28);
    assert(out[24] == 255 && out[25] == 255 && out[26] == 3 && !out[27]);
    printf("PICO_CAMERA_PROBE_PASS invalid=%u hardware=0\n", checks);
    return 0;
}
