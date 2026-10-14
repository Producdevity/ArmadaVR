#include "pico-camera-ife.h"
#include <math.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
bool pico_camera_ife_rdi_commands(const struct pico_camera_ife_rdi *c, void *output, size_t size)
{
    if (!c || !output || size < PICO_CAMERA_IFE_RDI_COMMAND_SIZE ||
        c->index > (c->lite ? 3U : 2U) || c->sample_pattern > 7 ||
        !c->batch_size || c->batch_size > 32) return false;
    uint8_t bytes[PICO_CAMERA_IFE_RDI_COMMAND_SIZE] = {0};
    uint32_t address = (c->lite ? 0x1260U : 0x9a60U) + c->index * 0x200;
    put32(bytes, 0x03000001);
    put32(bytes + 4, address);
    put32(bytes + 8, 1 | c->sample_pattern << 24);
    put32(bytes + 12, 0x03000002);
    put32(bytes + 16, address + 8);
    put32(bytes + 20, c->batch_size - 1);
    put32(bytes + 24, c->batch_size == 1 ? UINT32_MAX : UINT32_C(1) << (c->batch_size - 1));
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
bool pico_camera_ife_throttle_command(float utilization, void *output, size_t size)
{
    if (!isfinite(utilization) || utilization < 0 || utilization > 1 || !output ||
        size < PICO_CAMERA_IFE_THROTTLE_COMMAND_SIZE) return false;
    uint32_t pattern = utilization <= 0.25f ? 0x44440001 :
                       utilization <= 0.5f ? 0xaaaa0001 :
                       utilization <= 0.75f ? 0xeeee0001 : 0;
    uint8_t bytes[PICO_CAMERA_IFE_THROTTLE_COMMAND_SIZE] = {0};
    put32(bytes, 0x03000001);
    put32(bytes + 4, 0x10c);
    put32(bytes + 8, pattern);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
