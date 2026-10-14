#include "pico-camera-isp.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(int argc, char **argv)
{
    unsigned char memory[80], before[80];
    unsigned char *out = memory + 1;
    memset(memory, 0xa5, sizeof(memory));
    memcpy(before, memory, sizeof(memory));
    struct pico_camera_ife_clocks c = {1, 0, UINT64_MAX, false};
    assert(!pico_camera_ife_clock_blob(NULL, out, 60));
    assert(!pico_camera_ife_clock_blob(&c, NULL, 60));
    assert(!pico_camera_ife_clock_blob(&c, out, 59));
    c.left_pixel_hz = 0; assert(!pico_camera_ife_clock_blob(&c, out, 60));
    c.left_pixel_hz = 1; c.rdi0_hz = 0; assert(!pico_camera_ife_clock_blob(&c, out, 60));
    c.rdi0_hz = 1; c.right_pixel_hz = 1; assert(!pico_camera_ife_clock_blob(&c, out, 60));
    c.dual = true; c.right_pixel_hz = 0; assert(!pico_camera_ife_clock_blob(&c, out, 60));
    assert(!pico_camera_ife_hfr_blob(0, false, out, 36));
    assert(!pico_camera_ife_hfr_blob(33, false, out, 36));
    assert(!pico_camera_ife_hfr_blob(UINT32_MAX, true, out, 36));
    assert(!pico_camera_ife_hfr_blob(1, false, NULL, 36));
    assert(!pico_camera_ife_hfr_blob(1, false, out, 35));
    assert(!memcmp(memory, before, sizeof(memory)));
    c.left_pixel_hz = UINT64_MAX; c.right_pixel_hz = UINT64_MAX; c.rdi0_hz = UINT64_MAX;
    assert(pico_camera_ife_clock_blob(&c, out, 60));
    assert(get32(out) == 0x3801 && get32(out+4) == 1 && get32(out+8) == 4);
    for (unsigned i = 12; i < 36; i++) assert(out[i] == 0xff);
    for (unsigned i = 36; i < 60; i++) assert(out[i] == 0);
    assert(memory[0] == 0xa5 && memory[61] == 0xa5);
    for (unsigned batch = 1; batch <= 32; ++batch) for (unsigned batched = 0; batched <= 1; ++batched) {
        memset(memory, 0xa5, sizeof(memory));
        assert(pico_camera_ife_hfr_blob(batch, batched, out, 36));
        assert(get32(out) == 0x2000 && get32(out+4) == 1 && get32(out+8) == 0);
        assert(get32(out+12) == 0x3006 && get32(out+24) == 1 && get32(out+32) == 0);
        assert(get32(out+16) == (batched ? UINT32_C(1) << (batch-1) : 1));
        assert(get32(out+20) == (batched ? batch-1 : 0));
        assert(get32(out+28) == (batched ? 0 : batch-1));
        assert(memory[0] == 0xa5 && memory[37] == 0xa5);
    }
    if (argc == 2 && !strcmp(argv[1], "--encode")) {
        unsigned kind, dual, batch, batched;
        uint64_t left, right;
        while (scanf("%u%u%" SCNu64 "%" SCNu64 "%u%u", &kind, &dual, &left, &right, &batch, &batched) == 6) {
            if (dual) {if (left < right) left = right; if (left < 350000000) left = 350000000; right = left;}
            c = (struct pico_camera_ife_clocks){left, right, 350000000, dual};
            size_t size = kind ? PICO_CAMERA_HFR_BLOB_SIZE : PICO_CAMERA_CLOCK_BLOB_SIZE;
            assert(kind ? pico_camera_ife_hfr_blob(batch, batched, out, size) : pico_camera_ife_clock_blob(&c, out, size));
            printf("%08x ", get32(out));
            for (size_t i = 4; i < size; ++i) printf("%02x", out[i]);
            putchar('\n');
        }
    } else puts("PICO_CAMERA_BLOB_PASS");
    return 0;
}
