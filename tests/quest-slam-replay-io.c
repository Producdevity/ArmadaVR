#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static const int fifo_fd = 1000000000;
static unsigned char packet[51];
static bool pending, opened;
static bool camera_pending, camera_opened;
static unsigned char shutter[113];
static int64_t clock_ns;
static unsigned opens, closes, reads, clocks;

void quest_replay_clock(int64_t value) { clock_ns = value; }
void quest_replay_feed(int64_t stamp, const double accel[3], const double gyro[3])
{
    assert(opened && !pending && stamp > 0);
    memset(packet, 0, sizeof(packet));
    packet[0] = 2; packet[1] = 12; packet[3] = 1;
    packet[12] = 80; packet[14] = 36;
    uint64_t ticks = (uint64_t)stamp / 1000;
    for (unsigned i=0; i<8; ++i) packet[15+i] = (unsigned char)(ticks >> (8*i));
    for (unsigned i=0; i<3; ++i) {
        float a=(float)(accel[i]/9.80665), g=(float)(gyro[i]/0.017453292519943295);
        memcpy(packet+23+4*i,&a,4); memcpy(packet+35+4*i,&g,4);
    }
    float temperature=25; memcpy(packet+47,&temperature,4); pending=true;
}
void quest_replay_shutter(int64_t stamp)
{
    assert(camera_opened && !camera_pending && stamp>0);
    memset(shutter,0,sizeof(shutter));shutter[0]=3;shutter[1]=21;shutter[3]=1;
    shutter[21]=254;shutter[23]=89;
    unsigned char *p=shutter+24;p[0]=6;p[1]=3;p[5]=176;
    uint64_t ticks=(uint64_t)stamp/1000;
    for(unsigned i=0;i<8;i++){p[6+i]=(unsigned char)(ticks>>(8*i));p[24+i]=p[6+i];}
    camera_pending=true;
}
void quest_replay_finish(void)
{ assert(!opened && !pending && !camera_opened && !camera_pending && opens==2 && closes==2 && reads>1000 && clocks>reads); }
int open(const char *path, int flags, ...)
{
    if (!strcmp(path,"/dev/syncboss_stream0")) {
        assert(!camera_opened && flags==(O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW));
        ++opens;if(opened){camera_opened=true;return fifo_fd+1;}
        opened=true;return fifo_fd;
    }
    mode_t mode=0;
    if ((flags&O_CREAT) || (flags&O_TMPFILE)==O_TMPFILE) {
        va_list ap; va_start(ap,flags); mode=(mode_t)va_arg(ap,int); va_end(ap);
    }
    return (int)syscall(SYS_openat,AT_FDCWD,path,flags,mode);
}
int fstat(int fd, struct stat *st)
{
    if (fd==fifo_fd || fd==fifo_fd+1) { assert(fd==fifo_fd?opened:camera_opened); memset(st,0,sizeof(*st)); st->st_mode=S_IFCHR; return 0; }
    return (int)syscall(SYS_fstat,fd,st);
}
int ioctl(int fd, unsigned long request, ...)
{
    assert((fd==fifo_fd?opened:fd==fifo_fd+1 && camera_opened) && request==0x40240a01UL);
    va_list ap; va_start(ap,request); const unsigned char *p=va_arg(ap,const void *); va_end(ap);
    for (unsigned i=0; i<36; ++i) assert(p[i]==(i==32?1:fd==fifo_fd?(i==10?1:0):(i==31?64:0)));
    return 0;
}
ssize_t read(int fd, void *data, size_t size)
{
    if(fd==fifo_fd+1) {
        assert(camera_opened && size==4096);++reads;
        if(!camera_pending){errno=EAGAIN;return -1;}
        memcpy(data,shutter,sizeof(shutter));camera_pending=false;return sizeof(shutter);
    }
    if (fd!=fifo_fd) return syscall(SYS_read,fd,data,size);
    assert(opened && size==4096); ++reads;
    if (!pending) { errno=EAGAIN; return -1; }
    memcpy(data,packet,sizeof(packet)); pending=false; return sizeof(packet);
}
int close(int fd)
{
    if(fd==fifo_fd+1){assert(camera_opened);camera_opened=false;++closes;return 0;}
    if (fd!=fifo_fd) return (int)syscall(SYS_close,fd);
    assert(opened); opened=false; ++closes; return 0;
}
int clock_gettime(clockid_t clock, struct timespec *value)
{
    Dl_info info={0};
    if (dladdr(__builtin_return_address(0),&info) && info.dli_fname && strstr(info.dli_fname,"libquest_tracking.so")) {
        assert(clock==CLOCK_MONOTONIC && clock_ns>0); ++clocks;
        value->tv_sec=clock_ns/1000000000; value->tv_nsec=clock_ns%1000000000; return 0;
    }
    return (int)syscall(SYS_clock_gettime,clock,value);
}
