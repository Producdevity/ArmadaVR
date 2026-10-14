#include "pico-camera.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8*i)); }
static void put64(uint8_t *p, uint64_t n) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(n >> (8*i)); }
static void fill(uint8_t *p)
{
    memset(p, 0, 95);
    p[0] = 3; put32(p+1, 3); put32(p+5, 2); put32(p+9, PICO_CAMERA_MONO8);
    put32(p+0xd, 100); put64(p+0x11, UINT64_MAX); put64(p+0x19, 1000000000);
    put64(p+0x21, 998000000); put64(p+0x29, 20171); put64(p+0x31, UINT64_MAX);
    put32(p+0x39, 6); put32(p+0x3d, 32); put32(p+0x41, 1); put32(p+0x55, 42);
    memcpy(p+89, "abcdef", 6);
}
static void reject(const uint8_t *p, size_t size)
{
    struct pico_camera_frame f, before;
    memset(&f, 0xa5, sizeof(f)); memcpy(&before, &f, sizeof(f));
    assert(!pico_camera_decode(p, size, &f)); assert(!memcmp(&before, &f, sizeof(f)));
}
static int decode(void)
{
    unsigned id; char hex[131073]; uint8_t raw[65536];
    while (scanf("%u %131072s", &id, hex) == 2) {
        size_t size = strlen(hex)/2; assert(strlen(hex) % 2 == 0);
        for (size_t i=0; i<size; ++i) { unsigned n; assert(sscanf(hex+2*i,"%2x",&n)==1); raw[i]=(uint8_t)n; }
        struct pico_camera_frame f;
        assert(pico_camera_decode(raw, size, &f));
        printf("%u %u %u %u %u %llu %lld %lld %lld %llu %zu %u %u %u ", id,
               f.camera_id, f.width, f.height, f.exposure_lines, (unsigned long long)f.timestamp,
               (long long)f.qtimer_ns, (long long)f.center_ns, (long long)f.line_ns,
               (unsigned long long)f.frame_number, f.size, f.gain, f.classification, f.extra);
        for (size_t i=0; i<f.size; ++i) printf("%02x",f.pixels[i]);
        putchar('\n');
    }
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--decode")) return decode();
    uint8_t storage[98], *raw=storage+1; fill(raw);
    struct pico_camera_frame f;
    assert(pico_camera_decode(raw,95,&f));
    assert(f.camera_id==3 && f.width==3 && f.height==2 && f.timestamp==UINT64_MAX);
    assert(f.frame_number==UINT64_MAX && f.qtimer_ns==1000000000 && f.center_ns==998000000);
    assert(f.line_ns==20171 && f.gain==32 && f.exposure_lines==100 && f.classification==1 && f.extra==42);
    assert(f.pixels==raw+89 && f.size==6 && !memcmp(f.pixels,"abcdef",6));
    reject(NULL,95); assert(!pico_camera_decode(raw,95,NULL));
    for (size_t size=0;size<95;++size) reject(raw,size);
    reject(raw,96); reject(raw,SIZE_MAX);
    const unsigned fields[]={1,5,9,0x39,0x41,0x45,0x49};
    const uint32_t bad[]={0,1,4097,UINT32_MAX,0x80000000};
    for (size_t i=0;i<sizeof(fields)/sizeof(*fields);++i)
        for (size_t j=0;j<sizeof(bad)/sizeof(*bad);++j) {
            uint8_t candidate[95]; memcpy(candidate,raw,95); put32(candidate+fields[i],bad[j]);
            if ((fields[i]==0x41 && bad[j]<=3) || ((fields[i]==0x45||fields[i]==0x49)&&bad[j]==0)) continue;
            reject(candidate,95);
        }
    for (unsigned at=0x19; at<=0x29; at+=8) {
        uint8_t candidate[95]; memcpy(candidate,raw,95); put64(candidate+at,UINT64_MAX); reject(candidate,95);
        if (at!=0x29) {put64(candidate+at,0); reject(candidate,95);}
    }
    int64_t center=-1;
    assert(pico_camera_center(1000000000,100,20171,123,&center) && center==998939327);
    assert(pico_camera_center(INT64_MAX,737,1,0,&center) && center==INT64_MAX-52000-368);
    const int64_t badtime[]={INT64_MIN,-1,0,52000,52001};
    for (unsigned i=0;i<5;++i) {center=-7; assert(!pico_camera_center(badtime[i],2,1,0,&center) && center==-7);}
    assert(!pico_camera_center(INT64_MAX,737,INT64_MAX,0,&center));
    assert(!pico_camera_center(1000000,0,10,0,&center));
    assert(!pico_camera_center(1000000,738,10,0,&center));
    assert(!pico_camera_center(1000000,1,0,0,&center));
    assert(!pico_camera_center(1000000,1,1,-1,&center));
    assert(!pico_camera_center(1000000,1,1,INT64_MAX,&center));
    assert(!pico_camera_center(1000000,1,1,0,NULL));
    uint8_t ids[]={0,1,2,3}; struct pico_camera_frame frames[4];
    for (unsigned i=0;i<4;++i) {frames[i]=f;frames[i].camera_id=i;frames[i].center_ns+=i*33333;}
    struct pico_camera_frame_set set;
    assert(pico_camera_head_set(frames,4,ids,100000,&set));
    assert(set.count==4 && set.first_ns==998000000 && set.last_ns==998099999);
    frames[3].center_ns++; assert(pico_camera_head_set(frames,4,ids,100000,&set));
    frames[3].center_ns++; assert(!pico_camera_head_set(frames,4,ids,100000,&set));
    frames[3].center_ns--; assert(!pico_camera_head_set(frames,4,ids,99999,&set));
    assert(pico_camera_head_set(frames,2,ids,100000,&set));
    assert(!pico_camera_head_set(frames,3,ids,100000,&set));
    assert(!pico_camera_head_set(frames,4,ids,-1,&set));
    assert(!pico_camera_head_set(frames,4,ids,100001,&set));
    for (unsigned i=0;i<4;++i) {
        struct pico_camera_frame saved=frames[i];
        frames[i].classification=0; assert(!pico_camera_head_set(frames,4,ids,100000,&set));
        frames[i]=saved;frames[i].camera_id=99;assert(!pico_camera_head_set(frames,4,ids,100000,&set));
        frames[i]=saved;frames[i].center_ns=0;assert(!pico_camera_head_set(frames,4,ids,100000,&set));
        frames[i]=saved;frames[i].size=7;assert(!pico_camera_head_set(frames,4,ids,100000,&set));
        frames[i]=saved;frames[i].line_ns=0;assert(!pico_camera_head_set(frames,4,ids,100000,&set));
        frames[i]=saved;
    }
    ids[3]=ids[0];frames[3].camera_id=ids[0]; assert(!pico_camera_head_set(frames,4,ids,100000,&set));
    puts("PICO_CAMERA_PASS"); return 0;
}
