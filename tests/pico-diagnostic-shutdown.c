#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static jmp_buf stopped;
static pid_t pid;
static unsigned int timer;
static void (*alarm_handler)(int);
static int mount_count, fail_mount, fail_signal, fail_statfs, opens, shutdowns;
static long root_type;
static const char *serial_value, *board_value, *command_value;

static pid_t test_getpid(void) { return pid; }

static void (*test_signal(int number, void (*handler)(int)))(int)
{
    assert(number == SIGALRM && handler);
    if (fail_signal) return SIG_ERR;
    alarm_handler = handler;
    return SIG_DFL;
}

static unsigned int test_alarm(unsigned int seconds)
{
    assert(alarm_handler && seconds > 0 && seconds <= 90);
    timer = seconds;
    return 0;
}

static int test_mount(const char *source, const char *target, const char *type,
                      unsigned long flags, const void *data)
{
    (void)source; (void)target; (void)type; (void)flags; (void)data;
    assert(alarm_handler && timer == 90);
    assert(++mount_count <= 3);
    return mount_count == fail_mount ? -1 : 0;
}

static int test_open(const char *path, int flags, ...)
{
    ++opens;
    if (!strcmp(path, "/dev/kmsg")) return -1;
    assert(flags == (O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    const char *value = NULL;
    if (!strcmp(path, "/etc/pico-serial")) value = serial_value;
    else if (!strcmp(path, "/proc/device-tree/compatible")) value = board_value;
    else if (!strcmp(path, "/proc/cmdline")) value = command_value;
    else assert(!"Unexpected file access before refusal");
    if (!value) { errno = ENOENT; return -1; }
    int pipes[2];
    assert(!pipe(pipes));
    assert(write(pipes[1], value, strlen(value)) == (ssize_t)strlen(value));
    assert(!close(pipes[1]));
    return pipes[0];
}

static int test_statfs(const char *path, struct statfs *status)
{
    assert(!strcmp(path, "/"));
    memset(status, 0, sizeof(*status));
    status->f_type = root_type;
    return fail_statfs ? -1 : 0;
}

static int test_reboot(int command)
{
    assert(command == RB_POWER_OFF);
    ++shutdowns;
    longjmp(stopped, 1);
}

static int test_pause(void) { longjmp(stopped, 2); }

#define getpid test_getpid
#define signal test_signal
#define alarm test_alarm
#define mount test_mount
#define open test_open
#define statfs(path, status) test_statfs(path, status)
#define reboot test_reboot
#define pause test_pause
#define main diagnostic_main
#include "../system/pico/diagnostic-init.c"
#undef main
#undef getpid
#undef signal
#undef alarm
#undef mount
#undef open
#undef statfs
#undef reboot
#undef pause

static void reset(void)
{
    pid = 1;
    timer = 0;
    alarm_handler = NULL;
    mount_count = fail_mount = fail_signal = fail_statfs = opens = shutdowns = 0;
    root_type = 0x01021994;
    serial_value = "test-device\n";
    board_value = "qcom,kona";
    command_value = "reframed.pico=diagnostic androidboot.serialno=test-device";
    used = 0;
    report[0] = 0;
    kernel_log = -1;
}

static void expect_shutdown(const char *name)
{
    int result = setjmp(stopped);
    if (!result) (void)diagnostic_main();
    if (result != 1 || shutdowns != 1 || (!fail_signal && timer != 90)) {
        fprintf(stderr, "%s: result=%d shutdowns=%d timer=%u\n", name, result, shutdowns, timer);
        exit(1);
    }
    assert(strstr(report, "REFRAMED_PICO_DIAGNOSTIC_REFUSED\n"));
    assert(strstr(report, "DIAGNOSTIC_POWEROFF\n"));
    assert(!strstr(report, "DIAGNOSTIC_BEGIN"));
}

int main(void)
{
    reset();
    pid = 2;
    assert(diagnostic_main() == 2);
    assert(!alarm_handler && !timer && !mount_count && !opens && !shutdowns);

    reset(); serial_value = NULL; expect_shutdown("missing serial");
    const char *serials[] = {"", "\n", "test-device", "bad serial\n", "bad/serial\n"};
    for (size_t i = 0; i < sizeof(serials) / sizeof(*serials); i++) {
        reset(); serial_value = serials[i]; expect_shutdown("invalid serial");
    }
    reset(); board_value = NULL; expect_shutdown("missing board");
    reset(); board_value = "other,board"; expect_shutdown("wrong board");
    reset(); command_value = NULL; expect_shutdown("missing command line");
    const char *commands[] = {"", "reframed.pico=diagnostic androidboot.serialno=other",
        "reframed.pico=diagnostic-test androidboot.serialno=test-device",
        "reframed.pico=diagnostic reframed.pico=diagnostic androidboot.serialno=test-device",
        "reframed.pico=diagnostic androidboot.serialno=test-device androidboot.serialno=other"};
    for (size_t i = 0; i < sizeof(commands) / sizeof(*commands); i++) {
        reset(); command_value = commands[i]; expect_shutdown("wrong command line");
    }
    reset(); board_value = "linux,dummy-virt"; expect_shutdown("physical serial on virtual board");
    reset(); serial_value = "qemu\n"; board_value = "linux,dummy-virt";
    expect_shutdown("physical token on virtual board");
    reset(); fail_statfs = 1; expect_shutdown("unknown root filesystem");
    reset(); root_type = 0xef53; expect_shutdown("persistent root filesystem");
    for (int i = 1; i <= 3; i++) {
        reset(); fail_mount = i; expect_shutdown("mount failure");
        assert(mount_count == i && !opens);
    }
    reset(); fail_signal = 1; expect_shutdown("alarm handler failure");
    assert(!timer && !mount_count && !opens);
    puts("PICO_DIAGNOSTIC_SHUTDOWN_PASS");
    return 0;
}
