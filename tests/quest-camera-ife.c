#include "quest-camera-ife.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned word(const unsigned char *p)
{ return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }
static void unchanged(const unsigned char *p, size_t size)
{ for (size_t i = 0; i < size; ++i) assert(p[i] == 0xa5); }
int main(void)
{
    unsigned char output[64]; unsigned cases = 0;
    const unsigned dimensions[] = {1, 2, 480, 481, 512, 513, 640, 1024, 1280, 4095, 4096};
    for (unsigned rdi = 0; rdi < 4; ++rdi) for (unsigned pattern = 0; pattern < 8; ++pattern)
    for (unsigned x = 0; x < sizeof(dimensions) / sizeof(*dimensions); ++x)
    for (unsigned y = 0; y < sizeof(dimensions) / sizeof(*dimensions); ++y) {
        struct quest_camera_ife_rdi c = {QUEST_CAMERA_IFE_TITAN636, rdi, pattern, dimensions[x], dimensions[y]};
        memset(output, 0xa5, sizeof(output));
        assert(quest_camera_ife_rdi_commands(&c, output + 1, sizeof(output) - 1));
        assert(output[0] == 0xa5); unchanged(output + 45, sizeof(output) - 45);
        const unsigned counts[] = {1, 2, 2}, offsets[] = {0x28, 0x48, 0x50};
        unsigned position = 1;
        for (unsigned i = 0; i < 3; ++i) {
            assert(word(output + position) == (3U << 24 | counts[i]));
            assert(word(output + position + 4) == 0x500 + rdi * 0x100 + offsets[i]);
            position += 8;
            if (!i) assert(word(output + position) == pattern << 24);
            if (i == 1) assert(word(output + position) == 1 && !word(output + position + 4));
            if (i == 2) {
                assert(word(output + position) == (dimensions[x] - 1) << 16);
                assert(word(output + position + 4) == (dimensions[y] - 1) << 16);
            }
            position += counts[i] * 4;
        }
        assert(position == 45); ++cases;
    }
    struct quest_camera_ife_rdi c = {QUEST_CAMERA_IFE_TITAN636, 0, 0, 640, 513};
    for (size_t size = 0; size < QUEST_CAMERA_IFE_RDI_SIZE; ++size) {
        memset(output, 0xa5, sizeof(output)); assert(!quest_camera_ife_rdi_commands(&c, output, size));
        unchanged(output, sizeof(output));
    }
    assert(!quest_camera_ife_rdi_commands(NULL, output, sizeof(output)));
    assert(!quest_camera_ife_rdi_commands(&c, NULL, sizeof(output)));
    for (unsigned i = 0; i < 9; ++i) {
        struct quest_camera_ife_rdi bad = c;
        switch (i) {
        case 0: bad.revision = 0x650100; break;
        case 1: bad.revision = 0; break;
        case 2: bad.rdi = 4; break;
        case 3: bad.rdi = UINT32_MAX; break;
        case 4: bad.sample_pattern = 8; break;
        case 5: bad.width = 0; break;
        case 6: bad.width = 4097; break;
        case 7: bad.height = 0; break;
        case 8: bad.height = 4097; break;
        }
        memset(output, 0xa5, sizeof(output)); assert(!quest_camera_ife_rdi_commands(&bad, output, sizeof(output)));
        unchanged(output, sizeof(output));
    }
    for (size_t size = 0; size < QUEST_CAMERA_IFE_COMMON_SIZE; ++size) {
        memset(output, 0xa5, sizeof(output)); assert(!quest_camera_ife_common_command(c.revision, output, size));
        unchanged(output, sizeof(output));
    }
    assert(!quest_camera_ife_common_command(0x650100, output, sizeof(output)));
    assert(!quest_camera_ife_common_command(c.revision, NULL, sizeof(output)));
    memset(output, 0xa5, sizeof(output)); assert(quest_camera_ife_common_command(c.revision, output + 1, sizeof(output) - 1));
    assert(output[0] == 0xa5 && word(output + 1) == 0x03000001 && word(output + 5) == 0x1060 && word(output + 9) == 0x1f);
    unchanged(output + 13, sizeof(output) - 13);
    printf("QUEST_CAMERA_IFE_PASS cases=%u hardware=0\n", cases);
}
