#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

static int qemu_verified;

static void require(int condition, const char *message)
{
    if (!condition) {
        printf("ARMADA_ROOT_FAIL %s\n", message);
        fflush(stdout);
        if (qemu_verified)
            reboot(RB_POWER_OFF);
        for (;;) pause();
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getpid() != 1)
        return 1;
    char data[4096] = {0};
    FILE *stream = fopen("/proc/device-tree/compatible", "r");
    require(stream && fgets(data, sizeof(data), stream) && !strcmp(data, "linux,dummy-virt"), "QEMU target");
    fclose(stream);
    qemu_verified = 1;
    stream = fopen("/proc/cmdline", "r");
    require(stream && fgets(data, sizeof(data), stream) && strstr(data, "armada.root_test=1"), "test opt-in");
    fclose(stream);
    struct statfs filesystem;
    require(statfs("/", &filesystem) == 0 && filesystem.f_type == 0x794c7630, "overlay root");
    stream = fopen("/proc/self/mountinfo", "r");
    require(stream != NULL, "mount info");
    while (fgets(data, sizeof(data), stream)) {
        if (strstr(data, " - overlay "))
            fputs(data, stdout);
    }
    fclose(stream);
    stream = fopen("/root-identity", "r");
    require(stream && fgets(data, sizeof(data), stream) && !strcmp(data, "armada-root-fixture\n"), "root identity");
    fclose(stream);
    int fd = open("/written-in-ram", O_WRONLY | O_CREAT | O_EXCL, 0600);
    require(fd >= 0 && write(fd, "volatile\n", 9) == 9 && fsync(fd) == 0, "writable overlay");
    close(fd);
    sync();
    stream = fopen("/sys/block/vda/stat", "r");
    unsigned long long reads, merges, sectors, milliseconds, writes;
    require(stream && fscanf(stream, "%llu %llu %llu %llu %llu", &reads, &merges, &sectors, &milliseconds, &writes) == 5,
            "block statistics");
    fclose(stream);
    printf("ROOT_BLOCK_WRITES=%llu\n", writes);
    require(writes == 0, "no backing-device writes");
    puts("ARMADA_ROOT_OVERLAY_PASS");
    reboot(RB_POWER_OFF);
    for (;;) pause();
}
