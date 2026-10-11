#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    const char *loader = "/opt/armada-vr/systemd/usr/lib/ld-linux-aarch64.so.1";
    char path[PATH_MAX], target[PATH_MAX];
    if (getpid() == 1) strcpy(path, "/usr/lib/systemd/systemd");
    else {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (n <= 0 || n >= (ssize_t)sizeof(path) - 1) return 126;
        path[n] = 0;
    }
    if (strncmp(path, "/usr/", 5) || strstr(path, "..") ||
        snprintf(target, sizeof(target), "/opt/armada-vr/systemd%s", path) >= (int)sizeof(target)) return 126;
    char **args = calloc((size_t)argc + 7, sizeof(char *));
    if (!args) return 126;
    args[0] = (char *)loader; args[1] = "--library-path";
    args[2] = "/opt/armada-vr/systemd/usr/lib:/opt/armada-vr/systemd/usr/lib/systemd";
    args[3] = "--argv0"; args[4] = argv[0]; args[5] = target;
    for (int i = 1; i < argc; i++) args[i + 5] = argv[i];
    execv(loader, args);
    perror("systemd runtime exec");
    return 126;
}
