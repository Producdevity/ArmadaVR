#define _GNU_SOURCE
#include "pico-fastrpc.h"
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

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            printf("PICO_FASTRPC_DRIVER_FAIL line=%d errno=%d expression=%s\n", __LINE__, errno,   \
                   #x);                                                                            \
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

static int test_transport(void)
{
    const char *nodes[] = {"/dev/adsprpc-smd", "/dev/adsprpc-smd-secure"};
    unsigned cases = 0;
    for (unsigned n = 0; n < 2; n++) {
        for (unsigned cycle = 0; cycle < 8; cycle++) {
            int dev = open(nodes[n], O_RDWR | O_CLOEXEC);
            CHECK(dev >= 0);
            const uint32_t modes[] = {0, 1, 2, 4};
            for (unsigned m = 0; m < 4; m++) {
                CHECK(pico_fastrpc_set_mode(dev, modes[m]) == 0);
                cases++;
            }
            uint64_t dsp_address = UINT64_MAX;
            CHECK(pico_fastrpc_map(dev, 0, 0, 0, 4096, &dsp_address) == EBADR);
            CHECK(dsp_address == UINT64_MAX);
            cases++;
            CHECK(pico_fastrpc_attach_sensors(dev) == -EINVAL);
            cases++;
            for (unsigned mode = 0; mode < 4; mode++) {
                uint32_t payload = 42, crc[64] = {0};
                struct pico_fastrpc_arg arg = {
                    .pointer = (uintptr_t)&payload,
                    .length = sizeof(payload),
                    .fd = mode ? 0 : -1,
                    .attributes = mode >= 2 ? PICO_FASTRPC_NOVA : 0,
                };
                CHECK(pico_fastrpc_invoke(dev, 17, 0x10000, &arg, 1, mode == 3 ? crc : NULL) ==
                      -ECHRNG);
                CHECK(payload == 42);
                cases++;
            }
            uint32_t info = 0xdeadbeef;
            CHECK(pico_fastrpc_select_domain(dev, 2, &info) == (n ? -ECONNREFUSED : -EACCES));
            CHECK(info == 0xdeadbeef);
            CHECK(pico_fastrpc_invoke(dev, 17, 0, NULL, 0, NULL) == (n ? -EBADR : -ECHRNG));
            cases += 2;
            CHECK(close(dev) == 0);
        }
    }
    printf("PICO_FASTRPC_DRIVER_PASS bits=%zu cases=%u\n", sizeof(void *) * 8, cases);
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
        !strstr(command, "armada.fastrpc_transport_test=1")) {
        puts("PICO_FASTRPC_DRIVER_REFUSED: diskless virtual test fixture required");
        return 2;
    }
    if (read_text("/proc/sys/kernel/tainted", before, sizeof(before)))
        return 2;
    int result = test_transport();
    if (!init)
        return result;
    const char *programs[] = {"/probe32", "/unit64", "/unit32"};
    for (unsigned i = 0; !result && i < 3; i++) {
        pid_t child = fork();
        if (!child) {
            execl(programs[i], programs[i], NULL);
            _exit(127);
        }
        int status;
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status))
            result = 1;
    }
    if (read_text("/proc/sys/kernel/tainted", after, sizeof(after)) || strcmp(before, after))
        result = 1;
    printf("PICO_FASTRPC_DRIVER_TAINT before=%s after=%s", before, after);
    puts(result ? "PICO_FASTRPC_DRIVER_FAILED" : "PICO_FASTRPC_DRIVER_ALL_PASS");
    sync();
    reboot(RB_POWER_OFF);
    return result;
}
