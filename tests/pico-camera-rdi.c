#include "pico-camera-isp.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void refused(struct pico_camera_rdi_buffer buffer)
{
    uint8_t out[PICO_CAMERA_RDI_IO_SIZE], before[sizeof(out)];
    memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
    assert(!pico_camera_rdi_buffer_configure(&buffer, out, sizeof(out)));
    assert(!memcmp(out, before, sizeof(out)));
}

int main(int argc, char **argv)
{
    struct pico_camera_rdi_buffer b = {17, 23, 640, 400, 640, 400, 8, PICO_CAMERA_RAW_MIPI, 256000};
    uint8_t out[PICO_CAMERA_RDI_IO_SIZE + 2], before[sizeof(out)];
    if (argc == 2 && !strcmp(argv[1], "--encode")) {
        unsigned format;
        while (scanf("%" SCNd32 "%" SCNd32 "%u%u%u%u%u%u%" SCNu64,
                     &b.memory_handle, &b.completion_fence, &b.width, &b.height,
                     &b.stride, &b.slice_height, &b.bits, &format, &b.allocation_size) == 9) {
            b.format = (enum pico_camera_raw_format)format;
            assert(pico_camera_rdi_buffer_configure(&b, out, sizeof(out)));
            for (size_t i = 0; i < PICO_CAMERA_RDI_IO_SIZE; ++i) printf("%02x", out[i]);
            putchar('\n');
        }
        assert(feof(stdin)); return 0;
    }
    assert(argc == 1);
    memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
    assert(!pico_camera_rdi_buffer_configure(NULL, out, sizeof(out)));
    assert(!pico_camera_rdi_buffer_configure(&b, NULL, sizeof(out)));
    for (size_t size = 0; size < PICO_CAMERA_RDI_IO_SIZE; ++size) {
        assert(!pico_camera_rdi_buffer_configure(&b, out + 1, size));
        assert(!memcmp(out, before, sizeof(out)));
    }
    assert(pico_camera_rdi_buffer_configure(&b, out + 1, PICO_CAMERA_RDI_IO_SIZE));
    static const uint8_t expected[PICO_CAMERA_RDI_IO_SIZE] = {
        [0] = 17, [24] = 128, [25] = 2, [28] = 144, [29] = 1,
        [32] = 128, [33] = 2, [36] = 144, [37] = 1,
        [168] = 2, [176] = 5, [180] = 8, [188] = 6, [189] = 48,
        [192] = 23, [224] = 2, [232] = 1, [240] = 1,
    };
    assert(!memcmp(out + 1, expected, sizeof(expected)) && out[0] == 0xa5 && out[257] == 0xa5);
#define BAD(field, value) do { struct pico_camera_rdi_buffer bad = b; bad.field = value; refused(bad); } while (0)
    BAD(memory_handle, 0); BAD(memory_handle, -1); BAD(completion_fence, 0); BAD(completion_fence, -1);
    BAD(width, 0); BAD(width, 4097); BAD(height, 0); BAD(height, 4097);
    BAD(stride, 639); BAD(stride, 641); BAD(stride, UINT32_MAX); BAD(slice_height, 399); BAD(slice_height, UINT32_MAX);
    BAD(allocation_size, 255999); BAD(bits, 0); BAD(bits, 16); BAD(format, PICO_CAMERA_RAW_PLAIN8);
    BAD(format, (enum pico_camera_raw_format)3);
    unsigned cases = 0;
    for (unsigned bits = 8; bits <= 10; bits += 2)
    for (unsigned format = 1; format <= 2; ++format)
    for (unsigned width = 1; width <= 4096; width = width == 1 ? 639 : width == 639 ? 4096 : 4097) {
        b.bits = bits; b.format = (enum pico_camera_raw_format)format; b.width = width;
        b.stride = format == PICO_CAMERA_RAW_PLAIN16 ? width * 2 : (width * bits + 7) / 8;
        b.stride = (b.stride + 15) & ~15U;
        b.allocation_size = (uint64_t)b.stride * b.slice_height;
        assert(pico_camera_rdi_buffer_configure(&b, out, sizeof(out)));
        --b.allocation_size; refused(b); ++cases;
    }
    printf("PICO_CAMERA_RDI_PASS matrix=%u\n", cases); return 0;
}
