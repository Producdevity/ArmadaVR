#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <linux/udmabuf.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
struct dma_buf_export_sync_file { uint32_t flags; int32_t fd; };
struct dma_buf_import_sync_file { uint32_t flags; int32_t fd; };
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR('b', 2, struct dma_buf_export_sync_file)
#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW('b', 3, struct dma_buf_import_sync_file)
#endif
struct sw_sync_create_fence_data { uint32_t value; char name[32]; int32_t fence; };
#define SW_SYNC_IOC_CREATE_FENCE _IOWR('W', 0, struct sw_sync_create_fence_data)
#define SW_SYNC_IOC_INC _IOW('W', 1, uint32_t)
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "DMA_BUF_SYNC_FAIL line %d: %s (errno=%d)\n", __LINE__, #expr, errno); \
    exit(1); \
} } while (0)

_Static_assert(sizeof(struct dma_buf_export_sync_file) == 8, "export ABI");
_Static_assert(sizeof(struct dma_buf_import_sync_file) == 8, "import ABI");
_Static_assert(DMA_BUF_IOCTL_EXPORT_SYNC_FILE == 0xc0086202UL, "export ioctl ABI");
_Static_assert(DMA_BUF_IOCTL_IMPORT_SYNC_FILE == 0x40086203UL, "import ioctl ABI");

static unsigned fd_count(void)
{
    DIR *dir = opendir("/proc/self/fd");
    CHECK(dir);
    unsigned count = 0;
    while (readdir(dir)) ++count;
    closedir(dir);
    return count;
}

static int buffer(void)
{
    int mem = memfd_create("dmabuf-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(mem >= 0 && ftruncate(mem, 4096) == 0);
    CHECK(fcntl(mem, F_ADD_SEALS, F_SEAL_SHRINK) == 0);
    int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    CHECK(dev >= 0);
    struct udmabuf_create arg = {.memfd = mem, .flags = UDMABUF_FLAGS_CLOEXEC, .size = 4096};
    int fd = ioctl(dev, UDMABUF_CREATE, &arg);
    CHECK(fd >= 0);
    close(dev); close(mem);
    return fd;
}

static int timeline(void)
{
    int fd = open("/sys/kernel/debug/sync/sw_sync", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    return fd;
}

static int fence(int t, unsigned point)
{
    struct sw_sync_create_fence_data arg = {.value = point, .name = "dmabuf-test", .fence = -1};
    CHECK(ioctl(t, SW_SYNC_IOC_CREATE_FENCE, &arg) == 0 && arg.fence >= 0);
    return arg.fence;
}

static void advance(int t)
{
    uint32_t step = 1;
    CHECK(ioctl(t, SW_SYNC_IOC_INC, &step) == 0);
}

static void import(int buf, uint32_t flags, int fd)
{
    struct dma_buf_import_sync_file arg = {.flags = flags, .fd = fd};
    CHECK(ioctl(buf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &arg) == 0);
    CHECK(fcntl(fd, F_GETFD) >= 0);
}

static int snapshot(int buf, uint32_t flags)
{
    struct dma_buf_export_sync_file arg = {.flags = flags, .fd = -1};
    CHECK(ioctl(buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg) == 0);
    CHECK(arg.fd >= 0 && (fcntl(arg.fd, F_GETFD) & FD_CLOEXEC));
    return arg.fd;
}

static void pending(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    CHECK(poll(&p, 1, 0) == 0);
}

static void ready(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    CHECK(poll(&p, 1, 2000) == 1 && p.revents == POLLIN);
}

static void invalid_requests(int buf)
{
    unsigned start = fd_count();
    const uint32_t invalid[] = {0, DMA_BUF_SYNC_END, UINT32_MAX};
    int t = timeline(), f = fence(t, 1);
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        struct dma_buf_export_sync_file e = {.flags = invalid[i], .fd = -1};
        struct dma_buf_import_sync_file a = {.flags = invalid[i], .fd = f};
        CHECK(ioctl(buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &e) == -1 && errno == EINVAL);
        CHECK(ioctl(buf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &a) == -1 && errno == EINVAL);
    }
    struct dma_buf_import_sync_file arg = {.flags = DMA_BUF_SYNC_RW, .fd = -1};
    CHECK(ioctl(buf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &arg) == -1 && errno == EINVAL);
    arg.fd = buf;
    CHECK(ioctl(buf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &arg) == -1 && errno == EINVAL);
    CHECK(ioctl(buf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, (void *)1) == -1 && errno == EFAULT);
    CHECK(ioctl(buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, (void *)1) == -1 && errno == EFAULT);
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    *(struct dma_buf_export_sync_file *)page = (struct dma_buf_export_sync_file){.flags = DMA_BUF_SYNC_RW, .fd = -1};
    CHECK(mprotect(page, 4096, PROT_READ) == 0);
    unsigned before = fd_count();
    for (int i = 0; i < 128; ++i)
        CHECK(ioctl(buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, page) == -1 && errno == EFAULT);
    CHECK(fd_count() == before);
    CHECK(munmap(page, 4096) == 0);
    advance(t); close(f); close(t);
    CHECK(fd_count() == start);
    puts("DMA_BUF_SYNC_INVALID_AND_COPYBACK_PASS");
}

static void independent_writers(void)
{
    unsigned start = fd_count();
    int buf = buffer(), a = timeline(), b = timeline();
    int fa = fence(a, 1), fb = fence(b, 1);
    import(buf, DMA_BUF_SYNC_WRITE, fa);
    int before = snapshot(buf, DMA_BUF_SYNC_READ);
    import(buf, DMA_BUF_SYNC_RW, fb);
    close(fa); close(fb);
    int after = snapshot(buf, DMA_BUF_SYNC_READ);
    advance(b);
    pending(before); pending(after);
    struct pollfd p = {.fd = buf, .events = POLLIN};
    CHECK(poll(&p, 1, 0) == 0);
    advance(a); ready(before); ready(after);
    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN));
    close(before); close(after); close(a); close(b); close(buf);
    CHECK(fd_count() == start);
    puts("DMA_BUF_SYNC_WRITERS_PASS");
}

static void readers_and_writer(void)
{
    unsigned start = fd_count();
    int buf = buffer(), a = timeline(), b = timeline(), c = timeline();
    int fa = fence(a, 1), fb = fence(b, 1), fc = fence(c, 1);
    import(buf, DMA_BUF_SYNC_READ, fa); import(buf, DMA_BUF_SYNC_READ, fb);
    int read_before = snapshot(buf, DMA_BUF_SYNC_READ), write_before = snapshot(buf, DMA_BUF_SYNC_WRITE);
    ready(read_before); pending(write_before);
    import(buf, DMA_BUF_SYNC_WRITE, fc);
    int read_after = snapshot(buf, DMA_BUF_SYNC_READ), write_after = snapshot(buf, DMA_BUF_SYNC_RW);
    pending(read_after); pending(write_after);
    advance(c);
    ready(read_after); pending(write_after); pending(write_before);
    advance(a); pending(write_after); pending(write_before);
    advance(b); ready(write_after); ready(write_before);
    close(read_before); close(write_before); close(read_after); close(write_after);
    close(fa); close(fb); close(fc); close(a); close(b); close(c); close(buf);
    CHECK(fd_count() == start);
    puts("DMA_BUF_SYNC_READ_WRITE_SEPARATION_PASS");
}

static void poll_independent_writer(void)
{
    int buf = buffer(), reader = timeline(), writer = timeline();
    int r = fence(reader, 1), w = fence(writer, 1);
    import(buf, DMA_BUF_SYNC_READ, r); import(buf, DMA_BUF_SYNC_WRITE, w);
    advance(reader);
    const short events[] = {POLLIN, POLLOUT, POLLIN | POLLOUT};
    for (unsigned i = 0; i < sizeof(events)/sizeof(*events); ++i) {
        struct pollfd p = {.fd = buf, .events = events[i]};
        CHECK(poll(&p, 1, 0) == 0);
    }
    advance(writer);
    struct pollfd p = {.fd = buf, .events = POLLIN | POLLOUT};
    CHECK(poll(&p, 1, 2000) == 1 && p.revents == (POLLIN | POLLOUT));
    close(r); close(w); close(reader); close(writer); close(buf);
    puts("DMA_BUF_SYNC_POLL_WRITER_PASS");
}

static void snapshot_isolation(void)
{
    int buf = buffer(), a = timeline(), b = timeline();
    int fa = fence(a, 1), fb = fence(b, 1);
    import(buf, DMA_BUF_SYNC_WRITE, fa);
    int old = snapshot(buf, DMA_BUF_SYNC_RW);
    import(buf, DMA_BUF_SYNC_WRITE, fb);
    int newer = snapshot(buf, DMA_BUF_SYNC_RW);
    advance(a); ready(old); pending(newer);
    advance(b); ready(newer);
    close(fa); close(fb); close(old); close(newer); close(a); close(b); close(buf);
    puts("DMA_BUF_SYNC_SNAPSHOT_PASS");
}

static void cross_process(void)
{
    unsigned start = fd_count();
    int buf = buffer(), t = timeline(), f = fence(t, 1), pair[2];
    import(buf, DMA_BUF_SYNC_WRITE, f);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        close(pair[0]); close(buf); close(t); close(f);
        char byte, control[CMSG_SPACE(sizeof(int))];
        struct iovec io = {.iov_base = &byte, .iov_len = 1};
        struct msghdr msg = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        CHECK(recvmsg(pair[1], &msg, MSG_CMSG_CLOEXEC) == 1 && !(msg.msg_flags & MSG_CTRUNC));
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        CHECK(c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(sizeof(int)));
        int received; memcpy(&received, CMSG_DATA(c), sizeof(received));
        int s = snapshot(received, DMA_BUF_SYNC_RW); pending(s);
        CHECK(write(pair[1], "p", 1) == 1);
        ready(s); close(s); close(received); close(pair[1]); _exit(0);
    }
    close(pair[1]);
    char byte = 'b', control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec io = {.iov_base = &byte, .iov_len = 1};
    struct msghdr msg = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &buf, sizeof(buf));
    CHECK(sendmsg(pair[0], &msg, 0) == 1);
    CHECK(read(pair[0], &byte, 1) == 1 && byte == 'p');
    close(buf); close(f); advance(t);
    int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(t); close(pair[0]); CHECK(fd_count() == start);
    puts("DMA_BUF_SYNC_CROSS_PROCESS_PASS");
}

static void repeated_imports(void)
{
    unsigned start = fd_count();
    for (int round = 0; round < 16; ++round) {
        int buf = buffer(), ts[64];
        for (unsigned i = 0; i < 64; ++i) {
            ts[i] = timeline(); int f = fence(ts[i], 1);
            import(buf, i % 3 ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ, f);
            close(f);
        }
        int s = snapshot(buf, DMA_BUF_SYNC_RW);
        struct sync_file_info info = {0};
        CHECK(ioctl(s, SYNC_IOC_FILE_INFO, &info) == 0 && info.num_fences == 64);
        for (int i = 63; i >= 0; --i) {
            pending(s); advance(ts[i]);
        }
        ready(s);
        for (unsigned i = 0; i < 64; ++i) close(ts[i]);
        close(s); close(buf);
        CHECK(fd_count() == start);
    }
    puts("DMA_BUF_SYNC_1024_IMPORTS_PASS");
}

static void descriptor_exhaustion(int buf)
{
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        struct rlimit limits = {.rlim_cur = 48, .rlim_max = 48};
        CHECK(setrlimit(RLIMIT_NOFILE, &limits) == 0);
        int last = -1, fd;
        while ((fd = open("/dev/null", O_RDONLY | O_CLOEXEC)) >= 0) last = fd;
        CHECK(errno == EMFILE && last >= 0);
        struct dma_buf_export_sync_file arg = {.flags = DMA_BUF_SYNC_RW, .fd = -1};
        CHECK(ioctl(buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg) == -1 && errno == EMFILE);
        close(last);
        int s = snapshot(buf, DMA_BUF_SYNC_RW); ready(s); close(s); _exit(0);
    }
    int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("DMA_BUF_SYNC_FD_EXHAUSTION_PASS");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(getpid() == 1);
    CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
    CHECK(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0);
    char command[4096] = {0}, compatible[128] = {0};
    FILE *stream = fopen("/proc/cmdline", "r");
    CHECK(stream && fgets(command, sizeof(command), stream)); fclose(stream);
    stream = fopen("/proc/device-tree/compatible", "r");
    CHECK(stream && fgets(compatible, sizeof(compatible), stream)); fclose(stream);
    CHECK(strcmp(compatible, "linux,dummy-virt") == 0);
    CHECK(strstr(command, "armada.dmabuf_test=1") || strstr(command, "armada.dmabuf_test=unsupported"));
    CHECK(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0);
    CHECK(mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL) == 0);
    int buf = buffer();
    if (strstr(command, "armada.dmabuf_test=unsupported")) {
        struct dma_buf_export_sync_file e = {.flags = DMA_BUF_SYNC_RW, .fd = -1};
        struct dma_buf_import_sync_file i = {.flags = DMA_BUF_SYNC_RW, .fd = -1};
        CHECK(ioctl(buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &e) == -1 && errno == ENOTTY);
        CHECK(ioctl(buf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &i) == -1 && errno == ENOTTY);
        puts("DMA_BUF_SYNC_BASELINE_UNSUPPORTED_PASS");
    } else {
        unsigned start = fd_count();
        for (unsigned flags = 1; flags <= 3; ++flags) {
            int s = snapshot(buf, flags); ready(s); close(s);
        }
        puts("DMA_BUF_SYNC_EMPTY_AND_ABI_PASS");
        invalid_requests(buf); independent_writers(); readers_and_writer(); poll_independent_writer();
        snapshot_isolation(); cross_process(); repeated_imports(); descriptor_exhaustion(buf);
        CHECK(fd_count() == start);
        puts("DMA_BUF_SYNC_ALL_PASS");
        if (access("/dma_buf_resv_test.ko", F_OK) == 0) {
            int module = open("/dma_buf_resv_test.ko", O_RDONLY | O_CLOEXEC);
            CHECK(module >= 0 && syscall(SYS_finit_module, module, "", 0) == 0);
            close(module);
            puts("DMA_BUF_SYNC_RESERVATION_MODULE_PASS");
        }
    }
    close(buf); sync(); CHECK(reboot(RB_POWER_OFF) == 0);
    return 0;
}
