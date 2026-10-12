#define _GNU_SOURCE
#include "ion-buffer.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

struct ion_heap {
    char name[32];
    uint32_t type, id, reserved[3];
};
struct ion_query {
    uint32_t count, reserved0;
    uint64_t heaps;
    uint32_t reserved1, reserved2;
};
_Static_assert(sizeof(struct ion_heap) == 52, "ION heap ABI");
_Static_assert(sizeof(struct ion_query) == 24, "ION query ABI");
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            printf("ION_BUFFER_FAIL line=%d errno=%d expression=%s\n", __LINE__, errno, #x);       \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int read_text(const char *path, char *buffer, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t bytes = read(fd, buffer, size - 1);
    close(fd);
    if (bytes < 0)
        return -1;
    buffer[bytes] = 0;
    return 0;
}

static int test_buffers(void)
{
    int ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
    CHECK(ion >= 0);
    struct ion_query query = {0};
    CHECK(ioctl(ion, _IOWR('I', 8, struct ion_query), &query) == 0 && query.count == 1);
    struct ion_heap heap = {0};
    query.heaps = (uint64_t)(uintptr_t)&heap;
    CHECK(ioctl(ion, _IOWR('I', 8, struct ion_query), &query) == 0 && query.count == 1 &&
          heap.type == 0 && heap.id == 25);
    printf("ION_HEAP name=%.32s type=%u id=%u\n", heap.name, heap.type, heap.id);
    const size_t sizes[] = {4096, 8192, 65536, 851968};
    unsigned int cases = 0;
    for (unsigned int cached = 0; cached < 2; cached++)
        for (unsigned int n = 0; n < 4; n++)
            for (unsigned int cycle = 0; cycle < 4; cycle++) {
                struct ion_buffer buffer = {.fd = -1};
                CHECK(ion_buffer_allocate(ion, 25, sizes[n], cached, &buffer) == 0);
                CHECK(buffer.size == sizes[n] && buffer.fd >= 0);
                int fd = buffer.fd;
                CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
                unsigned char *first = buffer.data;
                unsigned char *second =
                    mmap(NULL, sizes[n], PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                CHECK(second != MAP_FAILED);
                CHECK(ion_buffer_sync(&buffer, ION_BUFFER_WRITE, false) == 0);
                for (size_t i = 0; i < sizes[n]; i++)
                    CHECK(first[i] == 0);
                for (size_t i = 0; i < sizes[n]; i++)
                    first[i] = (unsigned char)(i * 31 + cycle);
                CHECK(ion_buffer_sync(&buffer, ION_BUFFER_WRITE, true) == 0);
                CHECK(ion_buffer_sync(&buffer, ION_BUFFER_READ, false) == 0);
                for (size_t i = 0; i < sizes[n]; i++)
                    CHECK(second[i] == (unsigned char)(i * 31 + cycle));
                CHECK(ion_buffer_sync(&buffer, ION_BUFFER_READ, true) == 0);
                CHECK(munmap(second, sizes[n]) == 0 && ion_buffer_release(&buffer) == 0);
                CHECK(buffer.fd == -1 && !buffer.data && !buffer.size);
                cases++;
            }
    CHECK(close(ion) == 0);
    printf("ION_BUFFER_PASS bits=%zu cases=%u\n", sizeof(void *) * 8, cases);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    int init = getpid() == 1;
    if (init) {
        if (mount("proc", "/proc", "proc", 0, NULL) || mount("sysfs", "/sys", "sysfs", 0, NULL) ||
            mount("devtmpfs", "/dev", "devtmpfs", 0, NULL))
            return 2;
        int log = open("/dev/kmsg", O_WRONLY);
        if (log < 0 || dup2(log, 1) < 0 || dup2(log, 2) < 0)
            return 2;
        if (log > 2)
            close(log);
    }
    char board[128], command[2048], before[64], after[64];
    if (read_text("/proc/device-tree/compatible", board, sizeof(board)) ||
        strcmp(board, "linux,dummy-virt") || read_text("/proc/cmdline", command, sizeof(command)) ||
        !strstr(command, "armada.ion_test=1")) {
        puts("ION_BUFFER_REFUSED: diskless virtual test fixture required");
        return 2;
    }
    if (read_text("/proc/sys/kernel/tainted", before, sizeof(before)))
        return 2;
    int result = test_buffers();
    if (!init)
        return result;
    if (!result) {
        pid_t child = fork();
        if (!child) {
            execl("/probe32", "probe32", NULL);
            _exit(127);
        }
        int status;
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status))
            result = 1;
    }
    if (read_text("/proc/sys/kernel/tainted", after, sizeof(after)) || strcmp(before, after))
        result = 1;
    printf("ION_BUFFER_TAINT before=%s after=%s", before, after);
    puts(result ? "ION_BUFFER_FAILED" : "ION_BUFFER_ALL_PASS");
    sync();
    reboot(RB_POWER_OFF);
    return result;
}
