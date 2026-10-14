#include "pico-camera-isp.h"

#include <string.h>

static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

bool pico_camera_ife_acquire(const struct pico_camera_ife_config *c, void *output, size_t size)
{
    if (!c || !output || size < PICO_CAMERA_IFE_ACQUIRE_SIZE || c->phy > 5 || c->cphy > 1 ||
        c->lanes == 0 || c->lanes > (c->cphy ? 3U : 4U) || c->lane_map > UINT16_MAX ||
        c->virtual_channel > 3 || (c->bits != 8 && c->bits != 10) ||
        c->data_type != (c->bits == 8 ? 0x2aU : 0x2bU) ||
        !c->width || c->width > 4096 || !c->height || c->height > 4096) return false;

    uint32_t input_format = c->bits == 8 ? 2 : 3;
    uint32_t output_format;
    switch (c->output) {
    case PICO_CAMERA_RAW_PLAIN8:
        if (c->bits != 8) return false;
        output_format = 12;
        break;
    case PICO_CAMERA_RAW_PLAIN16:
        output_format = c->bits == 8 ? 13 : 14;
        break;
    case PICO_CAMERA_RAW_MIPI:
        output_format = input_format;
        break;
    default: return false;
    }

    /* Stock includes an unused output-port-sized tail and the acquire header's
     * data member in its allocation. Keep those bytes zero and retain its size. */
    uint8_t bytes[PICO_CAMERA_IFE_ACQUIRE_SIZE] = {0};
    put32(bytes, 0x2000);
    put32(bytes + 8, 1);
    put32(bytes + 12, 0x200);
    put32(bytes + 16, 232);
    uint8_t *in = bytes + 24;
    put32(in, 0x4001 + c->phy);
    put32(in + 4, c->cphy);
    put32(in + 8, c->lanes);
    put32(in + 12, c->lane_map);
    put32(in + 16, c->virtual_channel);
    put32(in + 32, c->data_type);
    put32(in + 48, 1);
    put32(in + 52, input_format);
    put32(in + 68, c->width - 1);
    put32(in + 72, c->width);
    put32(in + 92, c->height - 1);
    put32(in + 96, c->height);
    put32(in + 120, 1);
    uint8_t *out = in + 152;
    put32(out, 0x3006);
    put32(out + 4, output_format);
    put32(out + 8, c->width);
    put32(out + 12, c->height);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
