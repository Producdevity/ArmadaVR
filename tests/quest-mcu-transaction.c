#include "quest-mcu-transaction.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static unsigned checks, allocations, sends, reads, releases, invalidations;
static int allocate_error, send_error, read_error, release_error, invalidated_error;
static uint8_t sequence, record[276], sent[255], payload[252];
static size_t record_size, sent_size;
static bool short_write;
static struct quest_mcu_transaction_config config;
#define CHECK(x) do { ++checks; assert(x); } while (0)
int ioctl(int fd, unsigned long op, ...)
{
    va_list ap; va_start(ap,op); uint8_t *s=va_arg(ap,void *); va_end(ap);
    CHECK(fd==31 && s);
    if(op==0x80010a02UL) { ++allocations; *s=sequence; if(allocate_error) {errno=allocate_error;return -1;} }
    else { CHECK(op==0x40010a03UL && *s==sequence); ++releases; if(release_error) {errno=release_error;return -1;} }
    return 0;
}
ssize_t write(int fd, const void *data, size_t size)
{
    CHECK(fd==31 && size>=3 && size<=255); ++sends; sent_size=size; memcpy(sent,data,size);
    if(send_error) {errno=send_error;return -1;}
    return (ssize_t)size-short_write;
}
ssize_t read(int fd, void *data, size_t size)
{
    CHECK(fd==32 && size==276); ++reads;
    if(read_error) {errno=read_error;return -1;}
    memcpy(data,record,record_size); return (ssize_t)record_size;
}
static void invalidate(void *p, uint64_t generation, int error)
{
    CHECK(p==&config && generation==42 && error<0); ++invalidations; invalidated_error=error;
}
static void reset(unsigned version, unsigned seq, size_t size)
{
    allocations=sends=reads=releases=invalidations=0;
    allocate_error=send_error=read_error=release_error=invalidated_error=0; short_write=false;
    sequence=(uint8_t)seq; memset(record,0,sizeof(record));
    unsigned header=version==2?12:21; record[0]=(uint8_t)version; record[1]=(uint8_t)header;
    record[header]=0x4e; record[header+1]=sequence; record[header+2]=(uint8_t)size;
    for(unsigned i=0;i<252;i++) payload[i]=(uint8_t)i;
    memcpy(record+header+3,payload,size); record_size=header+3+size;
    config=(struct quest_mcu_transaction_config){31,32,42,1000,2,0x4e,payload,size,size};
}
static struct quest_mcu_transaction *create(void)
{
    struct quest_mcu_transaction *t=NULL;
    CHECK(quest_mcu_transaction_create(&config,invalidate,&config,&t));
    return t;
}
static void failed(struct quest_mcu_transaction **t, int error, uint8_t *output)
{
    CHECK(invalidations==1 && invalidated_error==error);
    unsigned a=allocations,s=sends,r=reads,f=releases;
    CHECK(quest_mcu_transaction_poll(*t,42,101,output,config.reply_size)==error);
    CHECK(quest_mcu_transaction_start(*t,102)==error);
    quest_mcu_transaction_cancel(*t); quest_mcu_transaction_cancel(*t);
    CHECK(!quest_mcu_transaction_destroy(t));
    quest_mcu_transaction_closed(t); CHECK(!*t);
    CHECK(a==allocations && s==sends && r==reads && f==releases && invalidations==1);
    for(unsigned i=0;i<252;i++) CHECK(output[i]==0xa5);
}
int main(void)
{
    reset(2,1,60); struct quest_mcu_transaction *t=NULL; uint8_t output[252];
    CHECK(!quest_mcu_transaction_create(NULL,invalidate,&config,&t));
    CHECK(!quest_mcu_transaction_create(&config,NULL,&config,&t));
    CHECK(!quest_mcu_transaction_create(&config,invalidate,&config,NULL));
    CHECK(!quest_mcu_transaction_destroy(NULL));
    CHECK(quest_mcu_transaction_destroy(&t)); quest_mcu_transaction_closed(NULL);
    struct quest_mcu_transaction_config good=config;
#define BAD(expr) do { config=good; expr; CHECK(!quest_mcu_transaction_create(&config,invalidate,&config,&t) && !t); } while(0)
    BAD(config.command_fd=-1); BAD(config.reply_fd=-1); BAD(config.reply_fd=31);
    BAD(config.generation=0); BAD(config.command_type=0); BAD(config.reply_type=0);
    BAD(config.timeout_ns=0); BAD(config.timeout_ns=10000000001LL);
    BAD(config.payload_size=253); BAD(config.reply_size=253); BAD(config.payload=NULL);
    config=good; t=create();
    CHECK(!quest_mcu_transaction_create(&config,invalidate,&config,&t));
    CHECK(quest_mcu_transaction_poll(t,42,100,output,60)==-EINVAL);
    CHECK(quest_mcu_transaction_start(t,0)==-EINVAL);
    CHECK(quest_mcu_transaction_start(t,INT64_MAX)==-EINVAL);
    CHECK(quest_mcu_transaction_destroy(&t) && !allocations && !invalidations);
    t=create(); quest_mcu_transaction_cancel(t);
    CHECK(quest_mcu_transaction_start(t,100)==-ECANCELED);
    CHECK(quest_mcu_transaction_destroy(&t) && invalidations==1 && !allocations);
    for(unsigned version=2;version<=3;version++) for(unsigned seq=1;seq<255;seq++) {
        size_t size=(seq-1)%253; reset(version,seq,size); t=create();
        memset(payload,0xee,sizeof(payload));
        CHECK(!quest_mcu_transaction_start(t,100));
        CHECK(allocations==1 && sends==1 && !reads && !releases);
        CHECK(sent_size==size+3 && sent[0]==2 && sent[1]==seq && sent[2]==size);
        for(unsigned i=0;i<size;i++) CHECK(sent[i+3]==(uint8_t)i);
        CHECK(quest_mcu_transaction_start(t,101)==-EALREADY);
        CHECK(!quest_mcu_transaction_destroy(&t));
        memset(output,0xa5,sizeof(output)); read_error=EAGAIN;
        CHECK(quest_mcu_transaction_poll(t,42,101,output,size)==0);
        for(unsigned i=0;i<252;i++) CHECK(output[i]==0xa5);
        read_error=0; unsigned header=record[1]; record[header+1]=seq==254?1:(uint8_t)(seq+1);
        CHECK(quest_mcu_transaction_poll(t,42,102,output,size)==0 && !releases);
        record[header+1]=(uint8_t)seq;
        CHECK(quest_mcu_transaction_poll(t,42,1099,size?output:NULL,size)==1);
        for(unsigned i=0;i<252;i++) CHECK(output[i]==(i<size?(uint8_t)i:0xa5));
        CHECK(allocations==1 && sends==1 && reads==3 && releases==1 && !invalidations);
        CHECK(quest_mcu_transaction_poll(t,42,1099,output,size)==-EALREADY && reads==3);
        quest_mcu_transaction_cancel(t); CHECK(!invalidations);
        CHECK(quest_mcu_transaction_destroy(&t));
    }
    for(unsigned fault=0;fault<17;fault++) {
        reset(3,42,60); t=create(); memset(output,0xa5,sizeof(output)); int error=0;
        switch(fault) {
        case 0: allocate_error=EFAULT; error=-EFAULT; break;
        case 1: allocate_error=EINTR; error=-EINTR; break;
        case 2: sequence=0; error=-EPROTO; break;
        case 3: send_error=EINTR; error=-EINTR; break;
        case 4: short_write=true; error=-EIO; break;
        }
        int start=quest_mcu_transaction_start(t,100);
        if(fault<5) { CHECK(start==error && allocations==1 && !releases && !reads); failed(&t,error,output); continue; }
        CHECK(start==0);
        int64_t now=101; uint64_t generation=42;
        switch(fault) {
        case 5: now=1100; error=-ETIMEDOUT; break;
        case 6: now=99; error=-ERANGE; break;
        case 7: generation=43; error=-ESTALE; break;
        case 8: read_error=EINTR; error=-EINTR; break;
        case 9: record_size=0; error=-EPIPE; break;
        case 10: record[2]=1; error=-EPIPE; break;
        case 11: record[21]=0x7d; error=-EPROTO; break;
        case 12: record_size--; error=-EPROTO; break;
        case 13: release_error=EIO; error=-EIO; break;
        case 14: release_error=EINTR; error=-EINTR; break;
        case 15: quest_mcu_transaction_cancel(t); error=-ECANCELED; break;
        case 16: read_error=EAGAIN; CHECK(!quest_mcu_transaction_poll(t,42,500,output,60)); now=499; error=-ERANGE; break;
        }
        CHECK(quest_mcu_transaction_poll(t,generation,now,output,60)==error);
        CHECK(releases==(fault==13 || fault==14));
        if(fault==5 || fault==6 || fault==7 || fault==15) CHECK(!reads);
        failed(&t,error,output);
    }
    reset(3,42,60); t=create(); CHECK(!quest_mcu_transaction_start(t,100));
    CHECK(quest_mcu_transaction_poll(t,42,100,NULL,60)==-EINVAL);
    CHECK(quest_mcu_transaction_poll(t,42,100,output,59)==-EINVAL && !reads && !invalidations);
    CHECK(quest_mcu_transaction_poll(t,42,100,output,60)==1);
    CHECK(quest_mcu_transaction_destroy(&t));
    printf("QUEST_MCU_TRANSACTION_PASS checks=%u completions=509 faults=17 hardware=0\n",checks);
}
