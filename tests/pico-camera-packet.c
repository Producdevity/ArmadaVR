#include "pico-camera-isp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void refused(struct pico_camera_packet_config c)
{
    uint8_t out[600], before[600];
    memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
    assert(!pico_camera_packet_size(&c));
    assert(!pico_camera_packet_encode(&c, out, sizeof(out)));
    assert(!memcmp(out, before, sizeof(out)));
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(int argc, char **argv)
{
    bool emit = argc == 2 && !strcmp(argv[1], "--encode");
    assert(argc == 1 || emit);
    struct pico_camera_command_buffer commands[8];
    for (unsigned i = 0; i < 8; ++i)
        commands[i] = (struct pico_camera_command_buffer){100 + (int)i, 16, 1024, 128, 5, 3, 4096};
    struct pico_camera_rdi_buffer image = {17, 23, 640, 400, 640, 400, 8, PICO_CAMERA_RAW_MIPI, 256000};
    struct pico_camera_packet_config c = {PICO_CAMERA_PACKET_IFE_UPDATE, UINT64_C(0x102030405060708), commands, 2, 1, 128, &image};
    uint8_t out[506], before[sizeof(out)];
    memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
    assert(!pico_camera_packet_size(NULL));
    assert(!pico_camera_packet_encode(&c, NULL, sizeof(out)));
    assert(pico_camera_packet_size(&c) == 360);
    for (size_t size = 0; size < 360; ++size) {
        assert(!pico_camera_packet_encode(&c, out + 1, size));
        assert(!memcmp(out, before, sizeof(out)));
    }
    assert(pico_camera_packet_encode(&c, out + 1, 360) == 360);
    assert(out[0] == 0xa5 && out[361] == 0xa5);
    assert(get32(out + 1) == 0x0f000001 && get32(out + 5) == 360);
    assert(!memcmp(out + 9, "\10\7\6\5\4\3\2\1", 8));
    assert(get32(out + 29) == 2 && get32(out + 33) == 48 && get32(out + 37) == 1);
    assert(get32(out + 41) == 304 && get32(out + 45) == 0 && get32(out + 49) == 1 && get32(out + 53) == 128);
    assert(get32(out + 57) == 100 && get32(out + 81) == 101 && get32(out + 105) == 17);
#define BAD(field, value) do { struct pico_camera_packet_config bad = c; bad.field = value; refused(bad); } while (0)
#define BAD_CMD(field, value) do { struct pico_camera_command_buffer saved = commands[0]; commands[0].field = value; refused(c); commands[0] = saved; } while (0)
    BAD(commands, NULL); BAD(command_count, 0); BAD(command_count, 9); BAD(kernel_command_index, -1);
    BAD(kernel_command_index, 2); BAD(kernel_command_offset, 124); BAD(kernel_command_offset, 132);
    BAD(request_id, 0); BAD(image, NULL); BAD(opcode, (enum pico_camera_packet_opcode)0);
    BAD(opcode, PICO_CAMERA_PACKET_IFE_INITIALIZE);
    BAD_CMD(memory_handle, 0); BAD_CMD(memory_handle, -1); BAD_CMD(memory_handle, 17); BAD_CMD(memory_handle, 101);
    BAD_CMD(capacity, 0); BAD_CMD(capacity, 4); BAD_CMD(capacity, UINT32_MAX);
    BAD_CMD(length, 0); BAD_CMD(length, 129); BAD_CMD(offset, 15); BAD_CMD(offset, UINT32_MAX - 15);
    BAD_CMD(allocation_size, 1039); BAD_CMD(type, 0); BAD_CMD(type, 11);
    commands[1].type = 7; refused(c); commands[1].type = 5;
    commands[1].length = 1024; c.kernel_command_offset = 1024; refused(c);
    commands[1].length = 128; c.kernel_command_offset = 128;
    unsigned cases = 0;
    enum pico_camera_packet_opcode opcodes[] = {
        PICO_CAMERA_PACKET_SENSOR_STREAM_ON, PICO_CAMERA_PACKET_SENSOR_UPDATE,
        PICO_CAMERA_PACKET_SENSOR_INITIALIZE, PICO_CAMERA_PACKET_SENSOR_PROBE,
        PICO_CAMERA_PACKET_SENSOR_CONFIGURE, PICO_CAMERA_PACKET_SENSOR_STREAM_OFF,
        PICO_CAMERA_PACKET_PHY_CONFIGURE, PICO_CAMERA_PACKET_IFE_INITIALIZE, PICO_CAMERA_PACKET_IFE_UPDATE,
    };
    for (unsigned op = 0; op < sizeof(opcodes) / sizeof(opcodes[0]); ++op)
    for (unsigned count = 1; count <= 8; ++count)
    for (unsigned offset = 0; offset <= 64; offset += 32) {
        bool ife = op >= 7;
        c = (struct pico_camera_packet_config){opcodes[op], op == 1 || op == 8 ? UINT64_C(0x100000001) : 0,
             commands, count, ife ? (int)count - 1 : -1, ife ? 128 : 0, op == 8 ? &image : NULL};
        for (unsigned i = 0; i < 8; ++i) { commands[i].offset = offset; commands[i].type = ife ? 5 : 7; }
        size_t size = pico_camera_packet_size(&c); assert(size);
        assert(pico_camera_packet_encode(&c, out + 1, sizeof(out) - 2) == size);
        if (emit) { for (size_t i = 0; i < size; ++i) printf("%02x", out[1 + i]); putchar('\n'); }
        ++cases;
    }
    if (!emit) printf("PICO_CAMERA_PACKET_PASS matrix=%u\n", cases);
    return 0;
}
