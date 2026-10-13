#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("PICO_SSC_DRIVER_FAIL line=%d errno=%d expression=%s\n", __LINE__, errno, #condition); \
        return 1; \
    } \
} while (0)

static int read_text(const char *path, char *buffer, size_t capacity)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t count = read(fd, buffer, capacity - 1);
    close(fd);
    if (count < 0 || (size_t)count == capacity - 1) return -1;
    buffer[count] = 0;
    return 0;
}

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    size_t count = strlen(text);
    ssize_t written = write(fd, text, count);
    int result = written == (ssize_t)count ? 0 : written < 0 ? -errno : -EIO;
    close(fd);
    return result;
}

static int test_driver(void)
{
    const char *boot = "/sys/kernel/boot_slpi/boot";
    const char *restart = "/sys/kernel/boot_slpi/ssr";
    const char *invalid[] = {"2", "-1", "1junk", "0junk", "2147483648", "1\n2"};
    unsigned held = 0, refusals = 0;
    for (unsigned cycle = 0; cycle < 64; ++cycle) {
        for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
            CHECK(write_text(boot, invalid[i]) == -EINVAL); ++refusals;
        }
        CHECK(write_text(boot, "1\n") == 0);
        CHECK(write_text(boot, "1\n") == 0);
        CHECK(write_text(boot, "0\n") == 0);
        CHECK(write_text(restart, "1\n") == -EINVAL);
        int files[8];
        for (unsigned i = 0; i < 8; ++i) {
            files[i] = open("/dev/sensors", O_RDONLY | O_CLOEXEC);
            CHECK(files[i] >= 0); ++held;
        }
        CHECK(write_text(boot, "1\n") == 0);
        CHECK(write_text("/sys/bus/platform/drivers/sensors-ssc/unbind", "ssc-test\n") == 0);
        CHECK(access(boot, F_OK) < 0 && errno == ENOENT);
        CHECK(access("/dev/sensors", F_OK) < 0 && errno == ENOENT);
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t timer = 0;
            CHECK(ioctl(files[i], _IOR('d', 3, unsigned int *), &timer) == 0);
            CHECK(close(files[i]) == 0);
        }
        CHECK(write_text("/sys/bus/platform/drivers/sensors-ssc/bind", "ssc-test\n") == 0);
        CHECK(access(boot, F_OK) == 0);
        CHECK(access("/dev/sensors", F_OK) == 0);
    }
    printf("PICO_SSC_DRIVER_PASS cycles=64 held_descriptors=%u invalid_commands=%u\n", held, refusals);
    return 0;
}

int main(void)
{
    if (getpid() != 1) return 2;
    if (mount("proc", "/proc", "proc", 0, NULL) || mount("sysfs", "/sys", "sysfs", 0, NULL) ||
        mount("devtmpfs", "/dev", "devtmpfs", 0, NULL)) return 2;
    int log = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (log < 0 || dup2(log, 1) < 0 || dup2(log, 2) < 0) return 2;
    if (log > 2) close(log);
    setvbuf(stdout, NULL, _IONBF, 0);
    char board[128], command[2048], identity[64], before[64], after[64];
    if (read_text("/proc/device-tree/compatible", board, sizeof(board)) ||
        strcmp(board, "linux,dummy-virt") || read_text("/proc/cmdline", command, sizeof(command)) ||
        read_text("/proc/device-tree/ssc-test/reframed,test-only", identity, sizeof(identity)) ||
        strcmp(identity, "ssc-lifecycle") ||
        !strstr(command, " reframed.ssc_lifecycle_test=1 ") ||
        strstr(command, "reframed.pico=")) {
        puts("PICO_SSC_DRIVER_REFUSED"); return 2;
    }
    if (read_text("/proc/sys/kernel/tainted", before, sizeof(before))) return 2;
    int result = test_driver();
    if (read_text("/proc/sys/kernel/tainted", after, sizeof(after)) || strcmp(before, after)) result = 1;
    printf("PICO_SSC_DRIVER_TAINT before=%s after=%s", before, after);
    puts(result ? "PICO_SSC_DRIVER_FAILED" : "PICO_SSC_DRIVER_ALL_PASS");
    reboot(RB_POWER_OFF);
    return result;
}
