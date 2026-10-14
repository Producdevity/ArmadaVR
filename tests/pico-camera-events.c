#include "pico-camera-events.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put(unsigned char *p,uint64_t value,unsigned bytes)
{
    for(unsigned i=0;i<bytes;i++)p[i]=(unsigned char)(value>>(8*i));
}
int main(void)
{
    unsigned char raw[66];unsigned char *p=raw+1;
    memset(raw,0xa5,sizeof(raw));
    struct pico_camera_fence_event fence={0},old_fence={0};
    struct pico_camera_sof_event sof={0},old_sof={0};
    struct pico_camera_error_event error={0},old_error={0};
    assert(pico_camera_fence_payload(17,UINT64_MAX,INT64_MAX,p,24));
    assert(raw[0]==0xa5&&raw[25]==0xa5);put(p+4,2,4);
    assert(pico_camera_fence_event_decode(0x08000000,0,p,64,&fence)==PICO_CAMERA_EVENT_VALID);
    assert(fence.fence==17&&fence.status==2&&fence.generation==UINT64_MAX&&fence.request_id==INT64_MAX);
    put(p+4,3,4);assert(pico_camera_fence_event_decode(0x08000000,0,p,64,&fence)==PICO_CAMERA_EVENT_VALID);
    old_fence=fence;
    for(unsigned n=0;n<64;n++)assert(pico_camera_fence_event_decode(0x08000000,0,p,n,&fence)==PICO_CAMERA_EVENT_MALFORMED);
    put(p+4,1,4);assert(pico_camera_fence_event_decode(0x08000000,0,p,64,&fence)==PICO_CAMERA_EVENT_MALFORMED);
    assert(!memcmp(&fence,&old_fence,sizeof(fence)));
    assert(pico_camera_fence_event_decode(0x08000000,2,NULL,0,NULL)==PICO_CAMERA_EVENT_UNRELATED);
    assert(pico_camera_fence_event_decode(0,0,NULL,0,NULL)==PICO_CAMERA_EVENT_UNRELATED);
    unsigned char saved[66];memcpy(saved,raw,sizeof(raw));
    assert(!pico_camera_fence_payload(0,1,1,p,24));assert(!pico_camera_fence_payload(1,0,1,p,24));
    assert(!pico_camera_fence_payload(1,1,0,p,24));assert(!pico_camera_fence_payload(1,1,UINT64_MAX,p,24));
    assert(!pico_camera_fence_payload(1,1,1,p,23));assert(!memcmp(raw,saved,sizeof(raw)));
    memset(raw,0xa5,sizeof(raw));put(p,17,4);put(p+8,INT64_MAX,8);put(p+16,UINT64_MAX,8);
    put(p+24,INT64_MAX,8);put(p+32,19,4);put(p+36,0,4);
    assert(pico_camera_sof_event_decode(0x08000000,0,p,64,&sof)==PICO_CAMERA_EVENT_VALID);
    assert(sof.session==17&&sof.link==19&&sof.request_id==INT64_MAX&&sof.frame_number==UINT64_MAX&&sof.qtimer_ns==INT64_MAX&&sof.status==0);
    old_sof=sof;
    for(unsigned n=0;n<64;n++)assert(pico_camera_sof_event_decode(0x08000000,0,p,n,&sof)==PICO_CAMERA_EVENT_MALFORMED);
    assert(pico_camera_sof_event_decode(0x08000000,2,p,64,&sof)==PICO_CAMERA_EVENT_UNRELATED);
    put(p+24,0,8);assert(pico_camera_sof_event_decode(0x08000000,0,p,64,&sof)==PICO_CAMERA_EVENT_MALFORMED);
    assert(!memcmp(&sof,&old_sof,sizeof(sof)));
    put(p+36,1,4);put(p+8,0,8);assert(pico_camera_sof_event_decode(0x08000000,0,p,64,&sof)==PICO_CAMERA_EVENT_VALID);
    assert(sof.status==1&&sof.qtimer_ns==0&&sof.request_id==0);
    memset(raw,0xa5,sizeof(raw));put(p,17,4);put(p+12,UINT32_MAX,4);put(p+16,0,4);put(p+20,19,4);put(p+24,UINT64_MAX,8);
    for(unsigned type=0;type<=5;type++) {
        put(p+8,type,4);assert(pico_camera_error_event_decode(0x08000000,1,p,64,&error)==PICO_CAMERA_EVENT_VALID);
        assert(error.type==type&&error.request_id==UINT32_MAX&&error.device==0&&error.link==19&&error.session==17&&error.resource_size==UINT64_MAX);
    }
    old_error=error;put(p+8,6,4);assert(pico_camera_error_event_decode(0x08000000,1,p,64,&error)==PICO_CAMERA_EVENT_MALFORMED);
    assert(!memcmp(&error,&old_error,sizeof(error)));
    for(unsigned n=0;n<64;n++)assert(pico_camera_error_event_decode(0x08000000,1,p,n,&error)==PICO_CAMERA_EVENT_MALFORMED);
    assert(pico_camera_error_event_decode(0x08000000,0,NULL,0,NULL)==PICO_CAMERA_EVENT_UNRELATED);
    puts("PICO_CAMERA_EVENTS_PASS");return 0;
}
