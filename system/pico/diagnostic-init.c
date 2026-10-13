#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

#define GADGET "/sys/kernel/config/usb_gadget/reframed_diagnostic"
#define PRIMARY_UDC "a600000.dwc3"
static char report[128 * 1024];
static size_t used;
static int kernel_log = -1;

static void record(const char *format, ...)
{
    if (used >= sizeof(report) - 1) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(report + used, sizeof(report) - used, format, args);
    va_end(args);
    if (n < 0) return;
    size_t added = (size_t)n < sizeof(report) - used ? (size_t)n : sizeof(report) - used - 1;
    if (kernel_log >= 0) {
        /* /dev/kmsg rejects writes longer than its per-record limit. */
        size_t at = 0;
        while (at < added) {
            size_t chunk = added - at < 900 ? added - at : 900;
            (void)write(kernel_log, report + used + at, chunk);
            at += chunk;
        }
    }
    used += added;
}

static ssize_t read_file(const char *path, char *data, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    ssize_t count = read(fd, data, size - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (count >= 0) data[count] = 0;
    return count;
}

static int token(const char *command, const char *expected)
{
    size_t length = strlen(expected);
    size_t key = strcspn(expected, "=") + 1;
    const char *at = command;
    int count = 0, keys = 0;
    while (*at) {
        at += strspn(at, " \t\r\n");
        size_t size = strcspn(at, " \t\r\n");
        if (size >= key && !memcmp(at, expected, key)) ++keys;
        if (size == length && !memcmp(at, expected, size)) ++count;
        at += size;
    }
    return count == 1 && keys == 1;
}

static _Noreturn void poweroff(int signal_number)
{
    (void)signal_number;
    reboot(RB_POWER_OFF);
    for (;;) pause();
}

static double seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now)) poweroff(0);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static int set(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t length = strlen(value);
    ssize_t count = write(fd, value, length);
    int result = count == (ssize_t)length ? 0 : -1;
    if (close(fd)) result = -1;
    return result;
}

static int gadget(const char *serial)
{
    if (mount("configfs", "/sys/kernel/config", "configfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)) return -1;
    const char *directories[] = {GADGET, GADGET "/strings/0x409", GADGET "/configs/c.1",
        GADGET "/configs/c.1/strings/0x409", GADGET "/functions/gser.usb0"};
    for (size_t i = 0; i < sizeof(directories) / sizeof(*directories); ++i)
        if (mkdir(directories[i], 0700)) return -1;
    return set(GADGET "/idVendor", "0x1d6b") || set(GADGET "/idProduct", "0x0104") ||
        set(GADGET "/bcdDevice", "0x0001") || set(GADGET "/bcdUSB", "0x0200") ||
        set(GADGET "/strings/0x409/serialnumber", serial) ||
        set(GADGET "/strings/0x409/manufacturer", "Reframed VR") ||
        set(GADGET "/strings/0x409/product", "Reframed VR Pico diagnostic") ||
        set(GADGET "/configs/c.1/strings/0x409/configuration", "Boot diagnostics") ||
        set(GADGET "/configs/c.1/bmAttributes", "0x80") ||
        set(GADGET "/configs/c.1/MaxPower", "250") ||
        symlink(GADGET "/functions/gser.usb0", GADGET "/configs/c.1/gser.usb0");
}

static int udc(const char *directory_path, char *name, size_t size)
{
    if (size <= strlen(PRIMARY_UDC)) return -1;
    name[0] = 0;
    DIR *directory = opendir(directory_path);
    if (!directory) return -1;
    struct dirent *entry;
    int found = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, PRIMARY_UDC)) { found = 1; break; }
    }
    closedir(directory);
    if (!found) return 0;
    /* Stock init selects the primary port; the secondary UDC can also exist. */
    const char *properties[] = {"compatible", "reg", "usb-core-id"};
    const char compatible[] = "snps,dwc3";
    const unsigned char reg[] = {0x0a, 0x60, 0, 0, 0, 0, 0xd9, 0x3c};
    const unsigned char core[] = {0, 0, 0, 0};
    const void *expected[] = {compatible, reg, core};
    const size_t lengths[] = {sizeof(compatible), sizeof(reg), sizeof(core)};
    for (size_t i = 0; i < sizeof(properties) / sizeof(*properties); ++i) {
        char path[512], value[64];
        if (snprintf(path, sizeof(path), "%s/%s/device/of_node/%s", directory_path,
                     PRIMARY_UDC, properties[i]) >= (int)sizeof(path)) return -1;
        ssize_t count = read_file(path, value, sizeof(value));
        if (count != (ssize_t)lengths[i] || memcmp(value, expected[i], lengths[i])) return -1;
    }
    strcpy(name, PRIMARY_UDC);
    return 1;
}

static int configured(const char *path)
{
    char state[64];
    ssize_t size = read_file(path, state, sizeof(state));
    if (size < 0) return -1;
    return size == (ssize_t)strlen("configured\n") && !memcmp(state, "configured\n", (size_t)size);
}

static int display_disabled(const char *sysfs)
{
    char path[512], value[8];
    if (snprintf(path, sizeof(path), "%s/module/msm_drm/parameters/modeset", sysfs) >= (int)sizeof(path)) return 0;
    if (read_file(path, value, sizeof(value)) != 2 || memcmp(value, "N\n", 2)) return 0;
    if (snprintf(path, sizeof(path), "%s/bus/platform/drivers/msm_drm", sysfs) >= (int)sizeof(path)) return 0;
    struct stat status;
    return lstat(path, &status) < 0 && errno == ENOENT;
}

static void snapshot(const char *path)
{
    char data[4096];
    ssize_t count = read_file(path, data, sizeof(data));
    if (count < 0) { record("UNAVAILABLE %s errno=%d\n", path, errno); return; }
    for (ssize_t i = 0; i < count; ++i)
        if ((unsigned char)data[i] < 32 && data[i] != '\n' && data[i] != '\t') data[i] = ' ';
    record("FILE %s\n%s\n", path, data);
}

static void blocks(void)
{
    DIR *directory = opendir("/sys/block");
    if (!directory) { record("UNAVAILABLE block inventory\n"); return; }
    struct dirent *entry;
    unsigned int count = 0;
    while ((entry = readdir(directory)) && count < 64) {
        if (entry->d_name[0] == '.') continue;
        ++count;
        char path[512];
        if (snprintf(path, sizeof(path), "/sys/block/%s/stat", entry->d_name) < (int)sizeof(path)) snapshot(path);
    }
    closedir(directory);
}

static void boot_log(void)
{
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    char data[8192];
    size_t end = used + 64 * 1024;
    int saved = kernel_log;
    kernel_log = -1;
    record("KERNEL_LOG_BEGIN\n");
    while (used < end && used < sizeof(report) - sizeof(data)) {
        ssize_t count = read(fd, data, sizeof(data) - 1);
        if (count < 0 && errno == EPIPE) continue;
        if (count <= 0) break;
        data[count] = 0;
        record("%s", data);
    }
    record("KERNEL_LOG_END\n");
    kernel_log = saved;
    close(fd);
}

static int transmit(double deadline)
{
    int fd = -1;
    while (seconds() < deadline && fd < 0) {
        fd = open("/dev/ttyGS0", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) usleep(100000);
    }
    if (fd < 0) return -1;
    struct termios options;
    if (tcgetattr(fd, &options)) { close(fd); return -1; }
    cfmakeraw(&options);
    options.c_cflag |= CLOCAL;
    if (tcsetattr(fd, TCSANOW, &options)) { close(fd); return -1; }
    size_t offset = 0;
    while (offset < used && seconds() < deadline) {
        size_t chunk = used - offset < 4096 ? used - offset : 4096;
        ssize_t count = write(fd, report + offset, chunk);
        if (count > 0) offset += (size_t)count;
        else if (count < 0 && errno != EAGAIN && errno != EINTR) break;
        else {
            struct pollfd wait = {.fd = fd, .events = POLLOUT};
            (void)poll(&wait, 1, 100);
        }
    }
    close(fd);
    return offset == used ? 0 : -1;
}

int main(void)
{
    if (getpid() != 1) return 2;
    if (signal(SIGALRM, poweroff) == SIG_ERR) goto refused;
    alarm(90);
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) ||
        mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) ||
        mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID | MS_NOEXEC, NULL)) goto refused;
    kernel_log = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    char board[256], command[4096], serial[80], expected[128];
    struct statfs filesystem;
    ssize_t serial_size = read_file("/etc/pico-serial", serial, sizeof(serial));
    if (serial_size <= 0 || serial[serial_size - 1] != '\n') goto refused;
    serial[--serial_size] = 0;
    if (!serial_size || strspn(serial, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.") != (size_t)serial_size) goto refused;
    if (read_file("/proc/device-tree/compatible", board, sizeof(board)) < 0 ||
        read_file("/proc/cmdline", command, sizeof(command)) < 0 || statfs("/", &filesystem) ||
        (filesystem.f_type != 0x858458f6 && filesystem.f_type != 0x01021994)) goto refused;
    int virtual = !strcmp(board, "linux,dummy-virt");
    snprintf(expected, sizeof(expected), "androidboot.serialno=%s", serial);
    if (virtual) {
        if (strcmp(serial, "qemu") || !token(command, "reframed.pico=diagnostic-test")) goto refused;
    } else if (strcmp(board, "qcom,kona") || !token(command, "reframed.pico=diagnostic") || !token(command, expected)) goto refused;
    if (virtual) alarm(10);
    if (!token(command, "msm_drm.modeset=0") || !display_disabled("/sys")) {
        record("DIAGNOSTIC_DISPLAY_GUARD_FAILED\nDIAGNOSTIC_POWEROFF\n");
        poweroff(0);
    }
    double deadline = seconds() + (virtual ? 2 : 70);
    record("REFRAMED_PICO_DIAGNOSTIC_BEGIN kind=%s serial=%s\n", virtual ? "virtual" : "physical", serial);
    record("DIAGNOSTIC_KMS_DISABLED\n");
    const char *paths[] = {"/proc/version", "/proc/cmdline", "/proc/device-tree/model",
        "/proc/device-tree/compatible", "/proc/meminfo", "/proc/self/mountinfo"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); ++i) snapshot(paths[i]);
    blocks();
    if (gadget(serial)) { record("DIAGNOSTIC_GADGET_FAILED errno=%d\n", errno); poweroff(0); }
    record("DIAGNOSTIC_GSER_CONFIGURED\n");
    char controller[256];
    int found;
    while ((found = udc("/sys/class/udc", controller, sizeof(controller))) == 0 && seconds() < deadline) usleep(100000);
    if (found != 1) {
        record("DIAGNOSTIC_UDC_UNAVAILABLE count=%d\nDIAGNOSTIC_POWEROFF\n", found);
        poweroff(0);
    }
    if (set(GADGET "/UDC", controller)) { record("DIAGNOSTIC_BIND_FAILED errno=%d\n", errno); poweroff(0); }
    record("DIAGNOSTIC_UDC_BOUND %s\n", controller);
    /* gser accepts buffered writes before USB configuration and drops them on close. */
    while ((found = configured("/sys/class/udc/" PRIMARY_UDC "/state")) == 0 && seconds() < deadline) usleep(100000);
    if (found != 1) {
        record("DIAGNOSTIC_USB_NOT_CONFIGURED\nDIAGNOSTIC_POWEROFF\n");
        poweroff(0);
    }
    record("DIAGNOSTIC_USB_CONFIGURED\n");
    boot_log();
    record("REFRAMED_PICO_DIAGNOSTIC_END\n");
    int sent = transmit(deadline);
    record("DIAGNOSTIC_USB_TRANSMIT result=%d\n", sent);
    sleep(2);
    int detached = set(GADGET "/UDC", "\n");
    record("DIAGNOSTIC_USB_DETACH result=%d\nDIAGNOSTIC_POWEROFF\n", detached);
    poweroff(0);
refused:
    record("REFRAMED_PICO_DIAGNOSTIC_REFUSED\nDIAGNOSTIC_POWEROFF\n");
    poweroff(0);
}
