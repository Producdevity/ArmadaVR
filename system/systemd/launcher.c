#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
#ifdef ARMADA_SYSTEMD_NATIVE_LIBRARIES
    const char *loader = "/lib/ld-linux-aarch64.so.1";
    const char *libraries = "/usr/lib64:/opt/armada-vr/systemd/usr/lib:/opt/armada-vr/systemd/usr/lib/systemd";
#else
    const char *loader = "/opt/armada-vr/systemd/usr/lib/ld-linux-aarch64.so.1";
    const char *libraries = "/opt/armada-vr/systemd/usr/lib:/opt/armada-vr/systemd/usr/lib/systemd";
#endif
    char path[PATH_MAX], target[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0 && n < (ssize_t)sizeof(path) - 1) path[n] = 0;
    else if (getpid() == 1 && n < 0 && errno == ENOENT) {
        if (!strcmp(argv[0], "/usr/lib/systemd/systemd-shutdown"))
            strcpy(path, "/usr/lib/systemd/systemd-shutdown");
        else if (!strcmp(argv[0], "/init") || !strcmp(argv[0], "/sbin/init") ||
                 !strcmp(argv[0], "/usr/lib/systemd/systemd"))
            strcpy(path, "/usr/lib/systemd/systemd");
        else return 126;
    } else return 126;
    if (strncmp(path, "/usr/", 5) || strstr(path, "..") ||
        snprintf(target, sizeof(target), "/opt/armada-vr/systemd%s", path) >= (int)sizeof(target)) return 126;
    char **args = calloc((size_t)argc + 7, sizeof(char *));
    if (!args) return 126;
    args[0] = (char *)loader; args[1] = "--library-path";
    args[2] = (char *)libraries;
    args[3] = "--argv0"; args[4] = argv[0]; args[5] = target;
    for (int i = 1; i < argc; i++) args[i + 5] = argv[i];
    execv(loader, args);
    perror("systemd runtime exec");
    return 126;
}
