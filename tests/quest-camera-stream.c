#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "quest-camera-stream.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef QUEST_MCU_VENDOR_TEST
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#include "syncboss.h"
#endif
#ifdef QUEST_CAMERA_STREAM_INSTALLED
#define quest_test_open open
#define quest_test_fstat fstat
#define quest_test_ioctl ioctl
#define quest_test_read read
#define quest_test_close close
#define quest_test_clock clock_gettime
#endif
static struct {
    uint8_t bytes[4096];
    size_t size;
    unsigned opens, closes, reads, filters, shutters, invalidations;
    int open_error, stat_error, filter_error, read_error, clock_error, invalid_error;
    bool regular, reject, close_error, repeat;
    int64_t now, read_delay, callback_delay;
    uint32_t bits[160];
    struct quest_camera_shutter values[160];
} mock;
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while(0)
int quest_test_open(const char *path, int flags, ...)
{
    ++mock.opens; CHECK(!strcmp(path,"/dev/syncboss_stream0"));
    CHECK(flags==(O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW));
    if(mock.open_error) {errno=mock.open_error;return -1;} return 73;
}
int quest_test_fstat(int fd, struct stat *st)
{
    CHECK(fd==73); if(mock.stat_error) {errno=mock.stat_error;return -1;}
    memset(st,0,sizeof(*st)); st->st_mode=mock.regular?S_IFREG:S_IFCHR;return 0;
}
int quest_test_ioctl(int fd, unsigned long command, ...)
{
    CHECK(fd==73 && command==0x40240a01UL);++mock.filters;
#ifdef QUEST_MCU_VENDOR_TEST
    CHECK(command==SYNCBOSS_SET_STREAMFILTER_IOCTL);
#endif
    va_list ap;va_start(ap,command);const uint8_t *p=va_arg(ap,const void *);va_end(ap);
    for(unsigned i=0;i<36;i++) CHECK(p[i]==(i==31?64:i==32?1:0));
    if(mock.filter_error) {errno=mock.filter_error;return -1;}return 0;
}
ssize_t quest_test_read(int fd, void *data, size_t size)
{
    CHECK(fd==73 && size==4096);++mock.reads;mock.now+=mock.read_delay;
    if(mock.read_error) {errno=mock.read_error;return -1;}
    if(!mock.size) {errno=EAGAIN;return -1;}
    memcpy(data,mock.bytes,mock.size);ssize_t n=(ssize_t)mock.size;
    if(!mock.repeat)mock.size=0;
    return n;
}
int quest_test_close(int fd)
{
    CHECK(fd==73);++mock.closes;
    if(mock.close_error) {errno=EINTR;return -1;}return 0;
}
int quest_test_clock(clockid_t clock, struct timespec *t)
{
    CHECK(clock==CLOCK_MONOTONIC);
    if(mock.clock_error) {errno=EIO;return -1;}
    t->tv_sec=mock.now/1000000000;t->tv_nsec=mock.now%1000000000;return 0;
}
static bool shutter(void *p, uint64_t generation, uint32_t bit,
                     const struct quest_camera_shutter_record *record, int64_t received)
{
    CHECK(p==&mock && generation==42 && mock.shutters<160 && received>0 && received<=mock.now);
    const struct quest_camera_shutter *value=&record->shutter;
    CHECK(record->nsync_offset_us==998500 && record->timestamp_ns==(int64_t)(value->timestamp_us+998500)*1000);
    CHECK(record->sof_ns==(int64_t)(value->sof_us+998500)*1000);
    mock.bits[mock.shutters]=bit;mock.values[mock.shutters++]=*value;
    mock.now+=mock.callback_delay;return !mock.reject;
}
static void invalidated(void *p, uint64_t generation, int error)
{
    CHECK(p==&mock && generation==42 && error<0 && !mock.invalidations);
    ++mock.invalidations;mock.invalid_error=error;
}
static void reset(void) {memset(&mock,0,sizeof(mock));mock.now=1000000000;}
static struct quest_camera_stream_config configuration(void)
{
    return (struct quest_camera_stream_config){42,{1,2,4,8},4,1000000,5000000,1000,&mock,shutter,invalidated};
}
static void put(uint8_t *p, uint64_t n, unsigned size)
{ for(unsigned i=0;i<size;i++) p[i]=(uint8_t)(n>>(8*i)); }
static size_t packet(uint8_t *p, unsigned version, unsigned type, uint32_t mask, uint64_t timestamp)
{
    unsigned header=version==2?12:21;memset(p,0,header+92);
    p[0]=(uint8_t)version;p[1]=(uint8_t)header;p[3]=1;put(p+4,998500,8);p[header]=254;p[header+2]=89;
    uint8_t *body=p+header+3;body[0]=(uint8_t)type;put(body+1,mask,4);body[5]=29;
    put(body+6,timestamp,8);put(body+14,31,4);put(body+18,4000,2);put(body+20,32,2);
    put(body+24,timestamp+4,8);return header+92;
}
static struct quest_camera_stream *start(void)
{
    struct quest_camera_stream *s=NULL;struct quest_camera_stream_config c=configuration();
    CHECK(quest_camera_stream_open(&c,&s));CHECK(mock.opens==1 && mock.filters==1);return s;
}
static void finish(struct quest_camera_stream **s, int error)
{
    CHECK(mock.invalidations==1 && mock.invalid_error==error && mock.closes==1);
    unsigned n=mock.reads;CHECK(quest_camera_stream_poll(*s)==error && mock.reads==n);
    quest_camera_stream_stop(*s);quest_camera_stream_destroy(s);CHECK(!*s && mock.closes==1 && mock.invalidations==1);
}
int main(void)
{
    reset();struct quest_camera_stream *s=NULL;struct quest_camera_stream_config c=configuration(),good=c;
    CHECK(!quest_camera_stream_open(NULL,&s));CHECK(!quest_camera_stream_open(&c,NULL));
    CHECK(quest_camera_stream_poll(NULL)==-EINVAL);quest_camera_stream_destroy(NULL);
#define BAD(expr) do {c=good;expr;CHECK(!quest_camera_stream_open(&c,&s) && !s && !mock.opens);} while(0)
    BAD(c.generation=0);BAD(c.camera_count=0);BAD(c.camera_count=5);BAD(c.camera_bits[0]=0);
    BAD(c.camera_bits[0]=3);BAD(c.camera_bits[1]=1);BAD(c.silence_ns=0);BAD(c.silence_ns=1000000001);
    BAD(c.max_age_ns=0);BAD(c.max_age_ns=1000000001);
    BAD(c.max_gap_us=0);BAD(c.max_gap_us=1000001);BAD(c.shutter=NULL);BAD(c.invalidated=NULL);
    for(unsigned fault=0;fault<5;fault++) {
        reset();c=good;
        if(fault==0)mock.open_error=EACCES;
        if(fault==1)mock.stat_error=EIO;
        if(fault==2)mock.regular=true;
        if(fault==3)mock.filter_error=ENOTTY;
        if(fault==4)mock.clock_error=EIO;
        CHECK(!quest_camera_stream_open(&c,&s) && !s && mock.opens==1);
        CHECK(mock.closes==(fault!=0) && !mock.invalidations);
    }
    const unsigned types[]={6,21,22};
    for(unsigned version=2;version<=3;version++)for(unsigned type=0;type<3;type++)for(uint32_t mask=1;mask<32;mask++) {
        reset();s=start();mock.size=packet(mock.bytes,version,types[type],mask,1000);
        unsigned count=0;for(unsigned i=0;i<4;i++)count+=(mask>>i)&1;
        CHECK(quest_camera_stream_poll(s)==(int)count && mock.shutters==count && !mock.invalidations);
        unsigned index=0;
        for(unsigned i=0;i<4;i++)if(mask&(1u<<i)) {
            const struct quest_camera_shutter *v=&mock.values[index];
            CHECK(mock.bits[index++]==(1u<<i) && v->timestamp_us==1000 && v->sof_us==1004);
            CHECK(v->type==types[type] && v->exposure_us==4000 && v->gain_code==32 && v->camera_mask==mask);
        }
        quest_camera_stream_stop(s);finish(&s,-ECANCELED);
    }
    reset();s=start();size_t n=0;
    for(unsigned i=0;i<32;i++)n+=packet(mock.bytes+n,i%2+2,types[i%3],15,1000+i*10);
    mock.size=n;CHECK(quest_camera_stream_poll(s)==128 && mock.shutters==128);
    quest_camera_stream_stop(s);finish(&s,-ECANCELED);
    for(unsigned fault=0;fault<22;fault++) {
        reset();s=start();mock.size=packet(mock.bytes,3,22,15,1000);int expected=-EPROTO;
        switch(fault) {
        case 0: mock.bytes[0]=4;break;
        case 1: mock.bytes[1]=20;break;
        case 2: mock.bytes[2]=1;expected=-EPIPE;break;
        case 3: mock.bytes[21]=80;break;
        case 4: mock.bytes[22]=1;break;
        case 5: mock.bytes[23]=253;break;
        case 6: mock.size--;break;
        case 7: mock.bytes[24]=7;break;
        case 8: put(mock.bytes+25,0,4);break;
        case 9: put(mock.bytes+30,0,8);break;
        case 10: mock.bytes[mock.size++]=0;break;
        case 11: mock.size+=packet(mock.bytes+mock.size,3,22,15,1000);expected=-ERANGE;break;
        case 12: mock.size+=packet(mock.bytes+mock.size,3,22,15,2001);expected=-ETIMEDOUT;break;
        case 13: mock.read_error=EIO;expected=-EIO;break;
        case 14: mock.read_delay=1000000;expected=-ETIMEDOUT;break;
        case 15: mock.now--;expected=-ERANGE;break;
        case 16: mock.clock_error=EIO;expected=-ERANGE;break;
        case 17: mock.bytes[3]=0;break;
        case 18: mock.bytes[3]=2;break;
        case 19: put(mock.bytes+4,1000000,8);expected=-ETIMEDOUT;break;
        case 20: put(mock.bytes+4,1,8);expected=-ETIMEDOUT;break;
        case 21: put(mock.bytes+48,2000,8);expected=-ETIMEDOUT;break;
        }
        CHECK(quest_camera_stream_poll(s)==expected && !mock.shutters);finish(&s,expected);
    }
    reset();s=start();mock.size=packet(mock.bytes,3,22,15,1000);mock.reject=true;
    CHECK(quest_camera_stream_poll(s)==-EPIPE && mock.shutters==1);finish(&s,-EPIPE);
    reset();s=start();mock.size=packet(mock.bytes,3,22,15,1000);mock.callback_delay=1000000;
    CHECK(quest_camera_stream_poll(s)==-ETIMEDOUT && mock.shutters==1);finish(&s,-ETIMEDOUT);
    reset();s=start();mock.read_error=EINTR;CHECK(!quest_camera_stream_poll(s) && mock.reads==8);
    mock.now+=1000000;mock.close_error=true;CHECK(quest_camera_stream_poll(s)==-ETIMEDOUT && mock.reads==8);finish(&s,-ETIMEDOUT);
    reset();s=start();mock.repeat=true;mock.size=packet(mock.bytes,3,22,16,1000);
    CHECK(!quest_camera_stream_poll(s) && mock.reads==8 && !mock.shutters);
    mock.now+=1000000;CHECK(quest_camera_stream_poll(s)==-ETIMEDOUT);finish(&s,-ETIMEDOUT);
    reset();s=start();mock.now+=500000;mock.size=packet(mock.bytes,3,22,7,1000);
    CHECK(quest_camera_stream_poll(s)==3);mock.now+=500000;mock.size=packet(mock.bytes,3,22,7,1010);
    CHECK(quest_camera_stream_poll(s)==-ETIMEDOUT && mock.shutters==3);finish(&s,-ETIMEDOUT);
    reset();s=start();mock.size=packet(mock.bytes,3,22,15,1000);CHECK(quest_camera_stream_poll(s)==4);
    mock.size=packet(mock.bytes,3,22,15,999);CHECK(quest_camera_stream_poll(s)==-ERANGE && mock.shutters==4);finish(&s,-ERANGE);
    reset();s=start();mock.size=packet(mock.bytes,3,22,15,1000);CHECK(quest_camera_stream_poll(s)==4);
    mock.size=packet(mock.bytes,3,22,15,1010);put(mock.bytes+4,998480,8);
    CHECK(quest_camera_stream_poll(s)==-ERANGE && mock.shutters==4);finish(&s,-ERANGE);
    printf("QUEST_CAMERA_STREAM_PASS checks=%u hardware=0\n",checks);
}
