#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Thermal test assertion failed: %s\n", message);
        puts("THERMAL_TEST_FAIL");
        fflush(stdout);
        for (;;) pause();
    }
}

static void temperature(int value, int expected)
{
    FILE *stream = fopen("/sys/module/armada_thermal_test/parameters/temperature", "w");
    require(stream != NULL, "temperature");
    require(fprintf(stream, "%d\n", value) > 0 && fclose(stream) == 0, "write temperature");
    usleep(500000);
    stream = fopen("/sys/module/armada_thermal_test/parameters/state", "r");
    require(stream != NULL, "cooling state");
    int actual = -1;
    require(fscanf(stream, "%d", &actual) == 1 && fclose(stream) == 0, "read cooling state");
    printf("THERMAL_SAMPLE temperature=%d expected=%d actual=%d\n", value, expected, actual);
    require(actual == expected, "cooling transition");
}

int main(int argc, char **argv)
{
    (void)argc;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getpid() == 1) {
        mkdir("/proc", 0755);
        mkdir("/sys", 0755);
        mkdir("/dev", 0755);
        require(mount("proc", "/proc", "proc", 0, NULL) == 0, "mount proc");
        require(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0, "mount sysfs");
        require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0, "mount dev");
    }
    char command[4096] = {0};
    FILE *stream = fopen("/proc/cmdline", "r");
    if (!stream || !fgets(command, sizeof(command), stream) ||
        !strstr(command, "armada.thermal_test=1"))
        return 1;
    fclose(stream);
    stream = fopen("/proc/device-tree/compatible", "r");
    char compatible[128] = {0};
    if (!stream || !fgets(compatible, sizeof(compatible), stream) || strcmp(compatible, "linux,dummy-virt"))
        return 1;
    fclose(stream);
    if (!strcmp(argv[0], "/sbin/poweroff")) {
        int console = open("/dev/console", O_WRONLY);
        if (console >= 0) {
            dprintf(console, "%s\n", strstr(command, "armada.thermal_stall=1") ?
                    "THERMAL_POWEROFF_STALLED" : "THERMAL_ORDERLY_POWEROFF_PASS");
            close(console);
        }
        if (strstr(command, "armada.thermal_stall=1"))
            for (;;) pause();
        sync();
        return reboot(RB_POWER_OFF);
    }
    require(getpid() == 1, "test must be init");
    int module = open("/armada_thermal_test.ko", O_RDONLY);
    require(module >= 0, "module");
    require(syscall(SYS_finit_module, module, "", 0) == 0, "finit_module");
    close(module);
    temperature(25000, 0);
    temperature(44999, 0);
    temperature(45000, 2);
    temperature(43001, 2);
    temperature(42999, 0);
    temperature(64999, 2);
    puts("THERMAL_BELOW_CRITICAL_PASS");
    stream = fopen("/sys/module/armada_thermal_test/parameters/temperature", "w");
    require(stream != NULL, "critical temperature");
    require(fputs("65000\n", stream) >= 0 && fclose(stream) == 0, "write critical");
    sleep(10);
    require(0, "critical shutdown did not occur");
    return 1;
}
