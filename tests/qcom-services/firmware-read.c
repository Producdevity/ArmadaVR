#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char class_path[PATH_MAX], parameter_path[PATH_MAX];
static const char *read_case;
static int firmware_fd = -1;
static unsigned reads;

static int fixture_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/sys/class/remoteproc"))
        return open(class_path, flags);
    if (!strcmp(path, "/sys/module/firmware_class/parameters/path"))
        return open(parameter_path, flags);
    return open(path, flags);
}

static int fixture_openat(int directory, const char *path, int flags, ...)
{
    int fd = openat(directory, path, flags);
    if (!strcmp(path, "remoteproc0/firmware"))
        firmware_fd = fd;
    return fd;
}

static ssize_t fixture_read(int fd, void *data, size_t length)
{
    if (fd != firmware_fd)
        return read(fd, data, length);
    firmware_fd = -1;
    reads++;
    if (!strcmp(read_case, "error")) {
        errno = EIO;
        return -1;
    }
    if (!strcmp(read_case, "empty"))
        return 0;
    if (!strcmp(read_case, "full")) {
        memset(data, 'x', length);
        return (ssize_t)length;
    }
    assert(!strcmp(read_case, "normal") && length >= 9);
    memcpy(data, "adsp.mdt\n", 9);
    return 9;
}

#define open fixture_open
#define openat fixture_openat
#define read fixture_read
#define main mapper_main
#include "pd-mapper.c"
#undef main
#undef read
#undef openat
#undef open

static void write_file(const char *path, const char *value)
{
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(value, file) >= 0);
    assert(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    read_case = argv[1];
    char directory[] = "/tmp/pd-firmware-read-XXXXXX";
    assert(mkdtemp(directory));
    assert(snprintf(class_path, sizeof(class_path), "%s/class", directory) > 0);
    assert(mkdir(class_path, 0700) == 0);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/class/remoteproc0", directory);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/class/remoteproc0/firmware", directory);
    write_file(path, "unused");
    snprintf(parameter_path, sizeof(parameter_path), "%s/path", directory);
    char value[PATH_MAX];
    snprintf(value, sizeof(value), "%s\n", directory);
    write_file(parameter_path, value);
    snprintf(path, sizeof(path), "%s/test.jsn", directory);
    write_file(path, "{}");
    struct assoc set;
    assoc_init(&set, 20);
    assert(pd_enumerate_jsons(&set) == 0);
    unsigned long cursor;
    const char *key;
    unsigned count = 0;
    assoc_foreach(key, NULL, &set, cursor) {
        char resolved[PATH_MAX];
        assert(realpath(key, resolved) && !strcmp(resolved, path));
        count++;
    }
    assoc_destroy(&set);
    assert(reads == 1);
    assert(count == (!strcmp(read_case, "normal") ? 1u : 0u));
    printf("PD_FIRMWARE_READ_PASS case=%s maps=%u\n", read_case, count);
    return 0;
}
