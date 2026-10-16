#include "quest-mcu-control.h"
#include "quest-camera-mcu.h"
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifdef QUEST_MCU_VENDOR_TEST
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#include "syncboss.h"
#endif

static unsigned calls,cases;
static int failure, positive;
static uint8_t allocated=1;
static bool releasing;
static ssize_t read_count,write_count;
static uint8_t record[277],written[255];
static size_t sent_size;
int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;va_start(ap,request);uint8_t *sequence=va_arg(ap,void *);va_end(ap);
    assert(fd==31 && sequence);calls++;
#ifdef QUEST_MCU_VENDOR_TEST
    assert(request==(releasing?SYNCBOSS_SEQUENCE_NUMBER_RELEASE_IOCTL:SYNCBOSS_SEQUENCE_NUMBER_ALLOCATE_IOCTL));
#else
    assert(request==(releasing?0x40010a03UL:0x80010a02UL));
#endif
    if(releasing)assert(*sequence==allocated);else {*sequence=allocated;}
    if(failure){errno=failure;return -1;}return positive;
}
ssize_t write(int fd,const void *data,size_t size)
{
    assert(fd==31 && data && size>=3 && size<=255);calls++;sent_size=size;memcpy(written,data,size);
    if(failure){errno=failure;return -1;}return write_count<0?(ssize_t)size:write_count;
}
ssize_t read(int fd,void *data,size_t size)
{
    assert(fd==32 && size==276);calls++;
    if(failure){errno=failure;return -1;}
    if(read_count>=0 && (size_t)read_count<=size)memcpy(data,record,(size_t)read_count);
    return read_count;
}
static void receive_bad(int expected)
{
    uint8_t output[60];memset(output,0xa5,sizeof(output));
    assert(quest_mcu_reply_receive(32,0x4e,allocated,output,sizeof(output))==expected);
    for(unsigned i=0;i<sizeof(output);i++)assert(output[i]==0xa5);
    cases++;
}
int main(void)
{
    for(unsigned seq=1;seq<255;seq++) {
        allocated=seq;uint8_t actual=0;releasing=false;
        assert(!quest_mcu_sequence_allocate(31,&actual) && actual==seq);
        releasing=true;assert(!quest_mcu_sequence_release(31,&actual) && !actual);cases++;
    }
    const int errors[]={EIO,EINTR,EFAULT,ENODEV,EAGAIN};
    allocated=42;
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        failure=errors[i];unsigned before=calls;uint8_t actual=0;releasing=false;
        assert(quest_mcu_sequence_allocate(31,&actual)==-failure && !actual && calls==before+1);
        releasing=true;actual=allocated;assert(quest_mcu_sequence_release(31,&actual)==-failure && actual==allocated);
        assert(quest_mcu_packet_send(31,2,allocated,"x",1)==-failure);
        receive_bad(failure==EAGAIN?0:-failure);cases++;
    }
    failure=0;positive=1;uint8_t seq=0;releasing=false;
    assert(quest_mcu_sequence_allocate(31,&seq)==-EPROTO && !seq);
    seq=allocated;releasing=true;assert(quest_mcu_sequence_release(31,&seq)==-EPROTO && seq==allocated);positive=0;
    for(unsigned value=0;value<=255;value+=255) {
        allocated=value;seq=0;releasing=false;
        assert(quest_mcu_sequence_allocate(31,&seq)==-EPROTO && !seq);cases++;
    }
    uint8_t payload[252];for(unsigned i=0;i<252;i++)payload[i]=i;
    write_count=-1;
    for(unsigned sequence=1;sequence<255;sequence++)for(size_t size=0;size<=252;size++) {
        assert(!quest_mcu_packet_send(31,2,sequence,payload,size));
        assert(sent_size==size+3 && written[0]==2 && written[1]==sequence && written[2]==size);
        assert(!memcmp(written+3,payload,size));cases++;
    }
    for(write_count=0;write_count<25;write_count++)assert(quest_mcu_packet_send(31,2,1,payload,22)==-EIO);
    write_count=26;assert(quest_mcu_packet_send(31,2,1,payload,22)==-EIO);write_count=-1;
    for(unsigned version=2;version<=3;version++)for(unsigned sequence=1;sequence<255;sequence++) {
        unsigned header=version==2?12:21;memset(record,0,sizeof(record));record[0]=version;record[1]=header;
        allocated=sequence;record[header]=0x4e;record[header+1]=sequence;record[header+2]=60;
        memcpy(record+header+3,payload,60);read_count=header+63;
        uint32_t ids[2]={1,4};uint8_t query[25],reply[60];struct quest_camera_mcu_parameters result[2];
        assert(quest_camera_metadata_query_encode(ids,2,allocated,query,sizeof(query)));
        assert(!quest_mcu_packet_send(31,query[0],query[1],query+3,query[2]) && !memcmp(written,query,sizeof(query)));
        assert(quest_mcu_reply_receive(32,0x4e,allocated,reply,60)==1 && !memcmp(reply,payload,60));
        assert(quest_camera_metadata_reply_decode(ids,2,reply,60,result,2));
        for(unsigned i=0;i<2;i++)for(unsigned column=0;column<3;column++) {
            uint32_t expected;memcpy(&expected,payload+column*20+4*i,4);assert(result[i].words[column]==expected);
        }
        record[header+1]=sequence==254?1:sequence+1;receive_bad(0);record[header+1]=sequence;
        record[header]=0x4f;receive_bad(-EPROTO);record[header]=0x4e;
        record[2]=1;receive_bad(-EPIPE);record[2]=0;cases++;
    }
    allocated=42;memset(record,0,sizeof(record));record[0]=3;record[1]=21;record[21]=0x4e;record[22]=allocated;record[23]=60;
    for(read_count=0;read_count<84;read_count++)receive_bad(read_count==0?-EPIPE:-EPROTO);
    read_count=85;receive_bad(-EPROTO);read_count=277;receive_bad(-EPROTO);read_count=84;
    const unsigned indices[]={0,1,2,21,22,23};
    const uint8_t invalid[]={4,22,2,0,255,253};
    for(unsigned i=0;i<6;i++) {uint8_t before=record[indices[i]];record[indices[i]]=invalid[i];receive_bad(-EPROTO);record[indices[i]]=before;}
    unsigned before=calls;seq=1;
    assert(quest_mcu_sequence_allocate(31,&seq)==-EINVAL);
    assert(quest_mcu_sequence_allocate(-1,&seq)==-EINVAL);
    assert(quest_mcu_sequence_allocate(31,NULL)==-EINVAL);
    seq=0;assert(quest_mcu_sequence_release(31,&seq)==-EINVAL);seq=255;assert(quest_mcu_sequence_release(31,&seq)==-EINVAL);
    assert(quest_mcu_packet_send(31,2,0,payload,1)==-EINVAL);
    assert(quest_mcu_packet_send(31,2,255,payload,1)==-EINVAL);
    assert(quest_mcu_packet_send(31,0,1,payload,1)==-EINVAL);
    assert(quest_mcu_packet_send(31,2,1,NULL,1)==-EINVAL);
    assert(quest_mcu_packet_send(31,2,1,payload,253)==-EINVAL);
    assert(quest_mcu_reply_receive(32,0,1,payload,1)==-EINVAL);
    assert(quest_mcu_reply_receive(32,1,0,payload,1)==-EINVAL);
    assert(quest_mcu_reply_receive(32,1,1,NULL,1)==-EINVAL);
    assert(quest_mcu_reply_receive(32,1,1,payload,253)==-EINVAL);
    assert(calls==before);
    printf("QUEST_MCU_CONTROL_PASS cases=%u syscalls=%u hardware=0\n",cases,calls);
}
