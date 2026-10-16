#include "quest-camera-mcu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    unsigned cases=0;
    uint32_t ids[5];uint8_t wire[27],payload[60];
    struct quest_camera_mcu_parameters values[5];
    for(unsigned count=1;count<=5;count++)for(unsigned start=0;start<32;start++)for(unsigned seq=1;seq<=254;seq++) {
        for(unsigned i=0;i<count;i++)ids[i]=UINT32_C(1)<<((start+i)%32);
        memset(wire,0xa5,sizeof(wire));assert(quest_camera_metadata_query_encode(ids,count,seq,wire+1,25));
        assert(wire[0]==0xa5 && wire[26]==0xa5 && wire[1]==2 && wire[2]==seq && wire[3]==22 && wire[4]==0x4e && wire[5]==count);
        for(unsigned i=0;i<5;i++) {
            uint32_t actual;memcpy(&actual,wire+6+4*i,4);assert(actual==(i<count?ids[i]:0));
        }
        for(unsigned i=0;i<60;i++)payload[i]=(uint8_t)(i+seq+start);
        assert(quest_camera_metadata_reply_decode(ids,count,payload,60,values,count));
        for(unsigned i=0;i<count;i++) {
            assert(values[i].camera_bit==ids[i]);
            for(unsigned col=0;col<3;col++){uint32_t expected;memcpy(&expected,payload+col*20+i*4,4);assert(values[i].words[col]==expected);}
        }
        cases++;
    }
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint8_t *guard=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(guard!=MAP_FAILED && !mprotect(guard+page,page,PROT_NONE));
    ids[0]=1;ids[1]=4;
    for(unsigned len=0;len<=60;len++) {
        uint8_t *data=guard+page-len;memset(data,0xa5,len);memset(values,0xa5,sizeof(values));
        assert(quest_camera_metadata_reply_decode(ids,2,data,len,values,2)==(len==60));
        if(len!=60)for(unsigned i=0;i<sizeof(values);i++)assert(((uint8_t *)values)[i]==0xa5);
        cases++;
    }
    for(unsigned len=0;len<25;len++) {
        memset(wire,0xa5,sizeof(wire));assert(!quest_camera_metadata_query_encode(ids,2,1,wire,len));
        for(unsigned i=0;i<sizeof(wire);i++)assert(wire[i]==0xa5);
        cases++;
    }
    for(unsigned test=0;test<8;test++) {
        ids[0]=1;ids[1]=4;size_t count=2;uint8_t seq=1;
        switch(test){case 0:count=0;break;case 1:count=6;break;case 2:ids[0]=0;break;
        case 3:ids[0]=3;break;case 4:ids[1]=1;break;case 5:seq=0;break;case 6:seq=255;break;case 7:ids[0]=UINT32_MAX;break;}
        memset(wire,0xa5,sizeof(wire));memset(values,0xa5,sizeof(values));
        assert(!quest_camera_metadata_query_encode(ids,count,seq,wire,sizeof(wire)));
        if(seq==1)assert(!quest_camera_metadata_reply_decode(ids,count,payload,60,values,count));
        for(unsigned i=0;i<sizeof(wire);i++)assert(wire[i]==0xa5);
        for(unsigned i=0;i<sizeof(values);i++)assert(((uint8_t *)values)[i]==0xa5);
        cases++;
    }
    ids[0]=1;ids[1]=4;
    assert(!quest_camera_metadata_reply_decode(ids,2,payload,59,values,2));
    assert(!quest_camera_metadata_reply_decode(ids,2,payload,61,values,2));
    assert(!quest_camera_metadata_reply_decode(ids,2,payload,60,values,1));
    assert(!quest_camera_metadata_reply_decode(ids,2,payload,60,values,3));
    assert(!quest_camera_metadata_reply_decode(ids,2,NULL,60,values,2));
    assert(!quest_camera_metadata_reply_decode(ids,2,payload,60,NULL,2));
    assert(!quest_camera_metadata_reply_decode(NULL,2,payload,60,values,2));
    assert(!quest_camera_metadata_query_encode(NULL,2,1,wire,25));
    assert(!quest_camera_metadata_query_encode(ids,2,1,NULL,25));
    assert(!munmap(guard,page*2));printf("QUEST_CAMERA_MCU_PASS cases=%u hardware=0\n",cases);
}
