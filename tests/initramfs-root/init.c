#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>

static int qemu_verified;

static void require(int condition, const char *message)
{
    if (!condition) {
        printf("REFRAMED_ROOT_FAIL %s\n", message);
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
    require(stream && fgets(data, sizeof(data), stream) && strstr(data, "reframed.root_test=1"), "test opt-in");
    int usb = strstr(data, "reframed.root_transport=usb-storage") != NULL;
    fclose(stream);
    struct statfs filesystem;
    require(statfs("/", &filesystem) == 0 && filesystem.f_type == 0x794c7630, "overlay root");
    struct stat device;
    const char *label = "/dev/disk/by-label/reframed-vr-root";
    require(stat(label, &device) == 0 && S_ISBLK(device.st_mode), "root block device");
    unsigned int root_major = major(device.st_rdev), root_minor = minor(device.st_rdev);
    char path[PATH_MAX], block[PATH_MAX], attribute[PATH_MAX + 16];
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u", root_major, root_minor);
    require(realpath(path, block) != NULL, "root sysfs path");
    if (usb)
        require(strstr(block, "/usb") != NULL, "root uses USB transport");
    printf("ROOT_DEVICE=%u:%u ROOT_SYSFS=%s\n", root_major, root_minor, block);
    stream = fopen("/proc/self/mountinfo", "r");
    require(stream != NULL, "mount info");
    int lower_found = 0;
    while (fgets(data, sizeof(data), stream)) {
        unsigned int mount_major, mount_minor;
        char flags[256];
        if (sscanf(data, "%*u %*u %u:%u %*s %*s %255s", &mount_major, &mount_minor, flags) == 3 &&
            root_major == mount_major && root_minor == mount_minor) {
            require(!strcmp(flags, "ro") || !strncmp(flags, "ro,", 3), "read-only lower mount");
            require(strstr(data, " - ext4 ") != NULL, "ext4 lower mount");
            lower_found++;
            fputs(data, stdout);
        }
        if (strstr(data, " - overlay "))
            fputs(data, stdout);
    }
    fclose(stream);
    printf("ROOT_LOWER_MOUNTS_VISIBLE=%d\n", lower_found);
    snprintf(attribute, sizeof(attribute), "%s/partition", block);
    if (access(attribute, F_OK) == 0) {
        char *slash = strrchr(block, '/');
        require(slash != NULL, "partition parent");
        *slash = 0;
    }
    stream = fopen("/root-identity", "r");
    require(stream && fgets(data, sizeof(data), stream) && !strcmp(data, "reframed-root-fixture\n"), "root identity");
    fclose(stream);
    int fd = open("/written-in-ram", O_WRONLY | O_CREAT | O_EXCL, 0600);
    require(fd >= 0 && write(fd, "volatile\n", 9) == 9 && fsync(fd) == 0, "writable overlay");
    close(fd);
    sync();
    snprintf(attribute, sizeof(attribute), "%s/stat", block);
    stream = fopen(attribute, "r");
    unsigned long long reads, merges, sectors, milliseconds, writes;
    require(stream && fscanf(stream, "%llu %llu %llu %llu %llu", &reads, &merges, &sectors, &milliseconds, &writes) == 5,
            "block statistics");
    fclose(stream);
    printf("ROOT_BLOCK_WRITES=%llu\n", writes);
    require(writes == 0, "no backing-device writes");
    puts("REFRAMED_ROOT_OVERLAY_PASS");
    reboot(RB_POWER_OFF);
    for (;;) pause();
}
