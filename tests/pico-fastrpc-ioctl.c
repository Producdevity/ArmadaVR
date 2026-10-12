#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

struct fastrpc_init {
    uint32_t flags;
    uintptr_t file;
    int filelen, filefd;
    uintptr_t mem;
    int memlen, memfd;
};
struct fastrpc_init_attrs { struct fastrpc_init init; int attrs, siglen; };
_Static_assert(sizeof(struct fastrpc_init) == (sizeof(void *) == 8 ? 40 : 28), "INIT ABI");
_Static_assert(sizeof(struct fastrpc_init_attrs) == (sizeof(void *) == 8 ? 48 : 36), "INIT_ATTRS ABI");

static int read_text(const char *path, char *buffer, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t bytes = read(fd, buffer, size - 1);
    close(fd);
    if (bytes < 0) return -1;
    buffer[bytes] = 0;
    return 0;
}

static int test_ioctls(void)
{
    const char *devices[] = {"/dev/adsprpc-smd", "/dev/adsprpc-smd-secure"};
    unsigned int cases = 0;
    for (unsigned int dev = 0; dev < 2; ++dev)
        for (unsigned int cycle = 0; cycle < 8; ++cycle)
            for (unsigned int flags = 0; flags < 4; ++flags)
                for (unsigned int attrs = 0; attrs < 2; ++attrs) {
                    int fd = open(devices[dev], O_RDWR | O_CLOEXEC);
                    if (fd < 0) {
                        printf("FASTRPC_IOCTL_FAIL open=%s errno=%d\n", devices[dev], errno);
                        return 1;
                    }
                    struct fastrpc_init_attrs request = {.init = {.flags = flags, .filefd = -1, .memfd = -1}, .attrs = attrs ? 8 : 0};
                    unsigned long commands[] = {_IOWR('R', 6, struct fastrpc_init), _IOWR('R', 10, struct fastrpc_init_attrs)};
                    for (unsigned int cmd = 0; cmd < 2; ++cmd) {
                        errno = 0;
                        int result = ioctl(fd, commands[cmd], &request);
                        int error = errno;
                        if (result != -1 || error != EINVAL) {
                            printf("FASTRPC_IOCTL_FAIL bits=%zu node=%u cmd=%u flags=%u attrs=%d result=%d errno=%d\n",
                                   sizeof(void *) * 8, dev, cmd, flags, request.attrs, result, error);
                            close(fd);
                            return 1;
                        }
                        ++cases;
                    }
                    if (close(fd)) return 1;
                }
    printf("FASTRPC_IOCTL_PASS bits=%zu cases=%u\n", sizeof(void *) * 8, cases);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    int init = getpid() == 1;
    if (init) {
        if (mount("proc", "/proc", "proc", 0, NULL) || mount("sysfs", "/sys", "sysfs", 0, NULL) ||
            mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)) return 2;
        int log = open("/dev/kmsg", O_WRONLY);
        if (log < 0 || dup2(log, 1) < 0 || dup2(log, 2) < 0) return 2;
        if (log > 2) close(log);
    }
    char board[128], command[2048], before[64], after[64];
    if (read_text("/proc/device-tree/compatible", board, sizeof(board)) || strcmp(board, "linux,dummy-virt") ||
        read_text("/proc/cmdline", command, sizeof(command)) || !strstr(command, "armada.fastrpc_test=1")) {
        puts("FASTRPC_IOCTL_REFUSED: diskless virtual test fixture required");
        return 2;
    }
    if (read_text("/proc/sys/kernel/tainted", before, sizeof(before))) return 2;
    int result = test_ioctls();
    if (!init) return result;
    if (!result) {
        pid_t child = fork();
        if (!child) { execl("/probe32", "probe32", NULL); _exit(127); }
        int status;
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) result = 1;
    }
    if (read_text("/proc/sys/kernel/tainted", after, sizeof(after)) || strcmp(before, after)) result = 1;
    printf("FASTRPC_IOCTL_TAINT before=%s after=%s", before, after);
    puts(result ? "FASTRPC_IOCTL_FAILED" : "FASTRPC_IOCTL_ALL_PASS");
    sync();
    reboot(RB_POWER_OFF);
    return result;
}
