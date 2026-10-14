#include "pico-camera-control.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

static unsigned calls, failures, bad_output;
static uint32_t expected_opcode, expected_size;
static uint64_t get(const void *data,unsigned offset,unsigned size)
{
    const unsigned char *p=(const unsigned char *)data+offset;uint64_t v=0;
    for(unsigned i=0;i<size;i++)v|=(uint64_t)p[i]<<(8*i);
    return v;
}
static void set32(void *data,unsigned offset,uint32_t value)
{
    unsigned char *p=(unsigned char *)data+offset;
    for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(8*i));
}
int ioctl(int fd,unsigned long request,...)
{
    va_list args;va_start(args,request);void *header=va_arg(args,void *);va_end(args);
    assert((fd==17||fd==18||fd==19)&&request==0xc01856c0UL);calls++;
    assert(get(header,0,4)==expected_opcode&&get(header,4,4)==expected_size);
    assert(get(header,8,4)==(fd==18?0:1)&&get(header,12,4)==0);
    void *p=(void *)(uintptr_t)get(header,16,8);assert(p);
    if(fd==18) {
        if(expected_opcode==0){assert(!strcmp(p,"capture"));set32(p,64,bad_output?0:303);}
        else if(expected_opcode==1){assert(get(p,64,4)==303);for(unsigned i=0;i<64;i++)assert(!((unsigned char *)p)[i]);}
        else {assert(expected_opcode==4&&get(p,0,4)==303&&get(p,4,4)==0&&get(p,8,8)==UINT64_MAX&&get(p,16,8)==INT64_MAX);}
    } else if(expected_opcode==0x10b) {
        assert(get(p,0,8)==0);set32(p,0,bad_output?0:101);
    } else if(expected_opcode==0x10d) {
        assert(get(p,0,4)==101&&get(p,4,4)==2&&get(p,8,4)==11&&get(p,12,4)==12);
        for(unsigned i=16;i<268;i++)assert(!((unsigned char *)p)[i]);
        set32(p,264,bad_output?0:202);
    } else if(expected_opcode==0x10f) {
        assert(get(p,0,4)==101&&get(p,4,4)==202&&get(p,8,4)==1&&get(p,12,4)==1);
        assert(get(p,16,8)==0&&get(p,24,8)==INT64_MAX);
    } else if(expected_opcode==0x110) {
        assert(get(p,0,4)==101&&get(p,4,4)==202);for(unsigned i=8;i<24;i++)assert(!((unsigned char *)p)[i]);
    } else if(expected_opcode==0x111) {
        assert(get(p,0,4)==101&&get(p,4,4)==1&&get(p,8,4)==2&&get(p,12,4)==202&&get(p,16,4)==203&&get(p,20,4)==203&&get(p,24,4)==0);
    } else if(expected_opcode==0x116) {
        assert(get(p,0,4)<=1&&get(p,4,4)==101&&get(p,8,4)==1&&get(p,24,4)==202);
        assert(!get(p,12,4)&&!get(p,16,8)&&!get(p,28,4));
    } else if(expected_opcode==0x105) {
        assert(get(p,0,4)==101&&get(p,4,4)==11&&get(p,8,8)==64&&get(p,16,8)==404);
    } else {
        assert(get(p,0,4)==101);
        assert(get(p,4,4)==(expected_opcode==0x10c?0:expected_opcode==0x10e?202:11));
    }
    if(failures){errno=EINTR;return -1;}return 0;
}
#define CALL(op,bytes,expression) do{expected_opcode=op;expected_size=bytes;assert((expression)==0);}while(0)
int main(void)
{
    int32_t session=0,link=0,fence=0,device=11,devices[2]={11,12},links[2]={202,203};
    CALL(0x10b,8,pico_camera_session_create(17,&session));assert(session==101);
    CALL(0x10d,268,pico_camera_link_create(17,session,devices,2,&link));assert(link==202);
    CALL(0x116,32,pico_camera_link_activate(17,session,link,true));
    CALL(0x111,28,pico_camera_links_synchronize(17,session,links,203));
    CALL(0,68,pico_camera_fence_create(18,"capture",&fence));assert(fence==303);
    CALL(4,24,pico_camera_fence_register(18,fence,UINT64_MAX,INT64_MAX));
    CALL(0x105,24,pico_camera_device_configure(19,session,device,404,64,120));
    CALL(0x10f,32,pico_camera_request_schedule(17,session,link,INT64_MAX,true,true));
    CALL(0x103,8,pico_camera_device_stream(19,session,device,true));
    CALL(0x110,24,pico_camera_requests_flush(17,session,link));
    CALL(0x104,8,pico_camera_device_stream(19,session,device,false));
    CALL(0x116,32,pico_camera_link_activate(17,session,link,false));
    unsigned before=calls;failures=1;expected_opcode=1;expected_size=68;
    assert(pico_camera_fence_destroy(18,&fence)==-EINTR&&fence==303&&calls==before+1);
    expected_opcode=0x10e;expected_size=8;assert(pico_camera_link_destroy(17,session,&link)==-EINTR&&link==202);
    expected_opcode=0x106;assert(pico_camera_device_release(19,session,&device)==-EINTR&&device==11);
    expected_opcode=0x10c;assert(pico_camera_session_destroy(17,&session)==-EINTR&&session==101);
    failures=0;
    CALL(1,68,pico_camera_fence_destroy(18,&fence));assert(!fence);
    CALL(0x10e,8,pico_camera_link_destroy(17,session,&link));assert(!link);
    CALL(0x106,8,pico_camera_device_release(19,session,&device));assert(!device);
    CALL(0x10c,8,pico_camera_session_destroy(17,&session));assert(!session);
    bad_output=1;expected_opcode=0x10b;expected_size=8;assert(pico_camera_session_create(17,&session)==-EPROTO&&!session);
    expected_opcode=0;expected_size=68;assert(pico_camera_fence_create(18,"capture",&fence)==-EPROTO&&!fence);
    expected_opcode=0x10d;expected_size=268;assert(pico_camera_link_create(17,101,devices,2,&link)==-EPROTO&&!link);
    bad_output=0;failures=1;expected_opcode=0x10b;expected_size=8;
    assert(pico_camera_session_create(17,&session)==-EINTR&&session==101);
    expected_opcode=0x10d;expected_size=268;
    assert(pico_camera_link_create(17,session,devices,2,&link)==-EINTR&&link==202);
    expected_opcode=0;expected_size=68;
    assert(pico_camera_fence_create(18,"capture",&fence)==-EINTR&&fence==303);
    failures=0;
    CALL(1,68,pico_camera_fence_destroy(18,&fence));
    CALL(0x10e,8,pico_camera_link_destroy(17,session,&link));
    CALL(0x10c,8,pico_camera_session_destroy(17,&session));
    before=calls;
    assert(pico_camera_session_create(-1,&session)==-EINVAL);
    assert(pico_camera_request_schedule(17,101,202,0,false,false)==-EINVAL);
    assert(pico_camera_request_schedule(17,101,202,UINT64_MAX,false,false)==-EINVAL);
    assert(pico_camera_device_configure(19,101,11,404,65,128)==-EINVAL);
    assert(pico_camera_device_configure(19,101,11,404,64,119)==-EINVAL);
    assert(pico_camera_device_configure(19,101,11,404,0,55)==-EINVAL);
    assert(pico_camera_link_create(17,101,devices,9,&link)==-EINVAL);
    devices[1]=11;assert(pico_camera_link_create(17,101,devices,2,&link)==-EINVAL);
    assert(pico_camera_links_synchronize(17,101,links,999)==-EINVAL);
    assert(pico_camera_fence_register(18,303,0,1)==-EINVAL);
    char long_name[65];memset(long_name,'x',64);long_name[64]=0;
    assert(pico_camera_fence_create(18,long_name,&fence)==-EINVAL);
    assert(calls==before);printf("PICO_CAMERA_CONTROL_PASS ioctls=%u hardware=0\n",calls);return 0;
}
