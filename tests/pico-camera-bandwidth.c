#include "pico-camera-isp.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint64_t get(const unsigned char *p, unsigned size)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)p[i] << (8*i);
    return value;
}

int main(int argc, char **argv)
{
    unsigned char memory[1200], before[1200], *out=memory+1;
    memset(memory, 0xa5, sizeof(memory)); memcpy(before, memory, sizeof(memory));
    struct pico_camera_ife_bandwidth b = {1, 1, 1};
    assert(!pico_camera_ife_bandwidth_blob(NULL, out, 1132));
    assert(!pico_camera_ife_bandwidth_blob(&b, NULL, 1132));
    assert(!pico_camera_ife_bandwidth_blob(&b, out, 1131));
    b.camnoc_bytes_per_second=0; assert(!pico_camera_ife_bandwidth_blob(&b, out, 1132));
    b.camnoc_bytes_per_second=1; b.mnoc_average_bytes_per_second=0;
    assert(!pico_camera_ife_bandwidth_blob(&b, out, 1132));
    b.mnoc_average_bytes_per_second=2;
    assert(!pico_camera_ife_bandwidth_blob(&b, out, 1132));
    assert(!memcmp(memory, before, sizeof(memory)));
    for (uint64_t vote=1; vote; vote<<=1) {
        b=(struct pico_camera_ife_bandwidth){vote, vote, UINT64_MAX};
        assert(pico_camera_ife_bandwidth_blob(&b, out, 1132));
        assert(get(out,4)==0x46809&&get(out+4,4)==0&&get(out+8,4)==10);
        for (unsigned i=0;i<20;i++) {
            const unsigned char *p=out+12+i*56;
            if(i>=10) {for(unsigned j=0;j<56;j++)assert(p[j]==0);continue;}
            assert(get(p,4)==(i>=4&&i<=7?3:1)&&get(p+4,4)==1&&get(p+8,4)==i&&get(p+12,4)==0);
            assert(get(p+16,8)==(i==4?vote:0)&&get(p+24,8)==(i==4?vote:0));
            assert(get(p+32,8)==(i==4?UINT64_MAX:0)&&get(p+40,8)==0&&get(p+48,8)==0);
        }
        assert(memory[0]==0xa5&&memory[1133]==0xa5);
    }
    if (argc==2&&!strcmp(argv[1],"--encode")) {
        while(scanf("%" SCNu64 "%" SCNu64 "%" SCNu64,&b.camnoc_bytes_per_second,&b.mnoc_average_bytes_per_second,&b.mnoc_peak_bytes_per_second)==3) {
            assert(pico_camera_ife_bandwidth_blob(&b,out,1132));printf("%08" PRIx64 " ",get(out,4));
            for(unsigned i=4;i<1132;i++)printf("%02x",out[i]);
            putchar('\n');
        }
    } else puts("PICO_CAMERA_BANDWIDTH_PASS");
    return 0;
}
