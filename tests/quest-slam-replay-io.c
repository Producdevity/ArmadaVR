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
void quest_replay_finish(void)
{ assert(!opened && !pending && opens==1 && closes==1 && reads>1000 && clocks>reads); }
int open(const char *path, int flags, ...)
{
    if (!strcmp(path,"/dev/syncboss_stream0")) {
        assert(!opened && flags==(O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW));
        opened=true; ++opens; return fifo_fd;
    }
    mode_t mode=0;
    if ((flags&O_CREAT) || (flags&O_TMPFILE)==O_TMPFILE) {
        va_list ap; va_start(ap,flags); mode=(mode_t)va_arg(ap,int); va_end(ap);
    }
    return (int)syscall(SYS_openat,AT_FDCWD,path,flags,mode);
}
int fstat(int fd, struct stat *st)
{
    if (fd==fifo_fd) { assert(opened); memset(st,0,sizeof(*st)); st->st_mode=S_IFCHR; return 0; }
    return (int)syscall(SYS_fstat,fd,st);
}
int ioctl(int fd, unsigned long request, ...)
{
    assert(fd==fifo_fd && opened && request==0x40240a01UL);
    va_list ap; va_start(ap,request); const unsigned char *p=va_arg(ap,const void *); va_end(ap);
    for (unsigned i=0; i<36; ++i) assert(p[i]==(i==10 || i==32 ? 1 : 0));
    return 0;
}
ssize_t read(int fd, void *data, size_t size)
{
    if (fd!=fifo_fd) return syscall(SYS_read,fd,data,size);
    assert(opened && size==4096); ++reads;
    if (!pending) { errno=EAGAIN; return -1; }
    memcpy(data,packet,sizeof(packet)); pending=false; return sizeof(packet);
}
int close(int fd)
{
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
