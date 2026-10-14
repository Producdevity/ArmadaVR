#include "pico-camera-ife.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static uint32_t get(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
int main(void)
{
    unsigned cases = 0;
    uint8_t output[32], before[32]; memset(before, 0xa5, sizeof(before));
    for (unsigned lite = 0; lite < 2; ++lite)
        for (unsigned index = 0; index < (lite ? 4U : 3U); ++index)
            for (unsigned pattern = 0; pattern < 8; ++pattern)
                for (unsigned batch = 1; batch <= 32; ++batch) {
                    struct pico_camera_ife_rdi c = {lite, index, pattern, batch};
                    memcpy(output, before, sizeof(output));
                    assert(pico_camera_ife_rdi_commands(&c, output + 1, 28));
                    assert(output[0] == 0xa5 && output[29] == 0xa5);
                    assert(get(output + 1) == 0x03000001 && get(output + 13) == 0x03000002);
                    assert(get(output + 5) == (lite ? 0x1260U : 0x9a60U) + index * 0x200);
                    assert(get(output + 9) == (1U | pattern << 24));
                    assert(get(output + 17) == get(output + 5) + 8 && get(output + 21) == batch - 1);
                    assert(get(output + 25) == (batch == 1 ? UINT32_MAX : UINT32_C(1) << (batch - 1)));
                    ++cases;
                }
    struct pico_camera_ife_rdi invalid[] = {{false,3,0,1},{true,4,0,1},{false,0,8,1},{false,0,0,0},{false,0,0,33}};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        memcpy(output, before, sizeof(output));
        assert(!pico_camera_ife_rdi_commands(invalid + i, output, sizeof(output)));
        assert(!memcmp(output, before, sizeof(output)));
    }
    struct pico_camera_ife_rdi valid = {false,0,0,1};
    assert(!pico_camera_ife_rdi_commands(&valid, output, 27));
    assert(!pico_camera_ife_rdi_commands(NULL, output, 28));
    assert(!pico_camera_ife_rdi_commands(&valid, NULL, 28));
    const float values[] = {0,0.25f,nextafterf(0.25f,1),0.5f,nextafterf(0.5f,1),0.75f,nextafterf(0.75f,1),1};
    const uint32_t patterns[] = {0x44440001,0x44440001,0xaaaa0001,0xaaaa0001,0xeeee0001,0xeeee0001,0,0};
    for (unsigned i = 0; i < 8; ++i) {
        assert(pico_camera_ife_throttle_command(values[i],output + 1,12));
        assert(get(output + 1) == 0x03000001 && get(output + 5) == 0x10c && get(output + 9) == patterns[i]);
    }
    const float bad[] = {-0.01f,1.01f,INFINITY,-INFINITY,NAN};
    for (unsigned i = 0; i < 5; ++i) {
        memcpy(output, before, sizeof(output));
        assert(!pico_camera_ife_throttle_command(bad[i],output,sizeof(output)));
        assert(!memcmp(output, before, sizeof(output)));
    }
    assert(!pico_camera_ife_throttle_command(1,output,11));
    assert(!pico_camera_ife_throttle_command(1,NULL,12));
    printf("PICO_CAMERA_IFE_PASS rdi=%u throttle=8\n",cases);
}
