#include "quest-camera-ife.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i)); }

bool quest_camera_ife_common_command(uint32_t revision, void *output, size_t size)
{
    if (revision != QUEST_CAMERA_IFE_TITAN636 || !output || size < QUEST_CAMERA_IFE_COMMON_SIZE) return false;
    uint8_t bytes[QUEST_CAMERA_IFE_COMMON_SIZE] = {0};
    put32(bytes, 0x03000001);
    put32(bytes + 4, 0x1060);
    put32(bytes + 8, 0x1f);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
bool quest_camera_ife_rdi_commands(const struct quest_camera_ife_rdi *c, void *output, size_t size)
{
    if (!c || c->revision != QUEST_CAMERA_IFE_TITAN636 || c->rdi > 3 || c->sample_pattern > 7 ||
        !c->width || c->width > 4096 || !c->height || c->height > 4096 ||
        !output || size < QUEST_CAMERA_IFE_RDI_SIZE) return false;
    uint8_t bytes[QUEST_CAMERA_IFE_RDI_SIZE] = {0};
    uint32_t base = 0x500 + c->rdi * 0x100;
    put32(bytes, 0x03000001);
    put32(bytes + 4, base + 0x28);
    put32(bytes + 8, c->sample_pattern << 24);
    put32(bytes + 12, 0x03000002);
    put32(bytes + 16, base + 0x48);
    put32(bytes + 20, 1);
    put32(bytes + 24, 0);
    put32(bytes + 28, 0x03000002);
    put32(bytes + 32, base + 0x50);
    put32(bytes + 36, (c->width - 1) << 16);
    put32(bytes + 40, (c->height - 1) << 16);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
