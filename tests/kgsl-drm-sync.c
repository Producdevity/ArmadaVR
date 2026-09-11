#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "tu_kgsl_drm_sync.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #expr, errno); exit(1); \
} } while (0)

struct sw_sync_create_fence_data { uint32_t value; char name[32]; int32_t fence; };
#define SW_SYNC_IOC_CREATE_FENCE _IOWR('W', 0, struct sw_sync_create_fence_data)
#define SW_SYNC_IOC_INC _IOW('W', 1, uint32_t)

static int64_t deadline(void)
{
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec + 2000000000;
}

static unsigned fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    CHECK(directory != NULL);
    unsigned count = 0;
    while (readdir(directory)) ++count;
    closedir(directory);
    return count;
}

static void check_pending(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    CHECK(poll(&p, 1, 0) == 0);
}

static void check_ready(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    CHECK(poll(&p, 1, 2000) == 1 && p.revents == POLLIN);
}

static int create_fence(int timeline, uint32_t point)
{
    struct sw_sync_create_fence_data data = {.value = point, .name = "kgsl-bridge-test", .fence = -1};
    CHECK(ioctl(timeline, SW_SYNC_IOC_CREATE_FENCE, &data) == 0);
    CHECK(data.fence >= 0);
    return data.fence;
}

static void roundtrip(const char *render_node, int drm, uint64_t point)
{
    unsigned start_fds = fd_count();
    int software = open("/sys/kernel/debug/sync/sw_sync", O_RDWR | O_CLOEXEC);
    CHECK(software >= 0);
    int fence = create_fence(software, 1);
    uint32_t source, destination;
    CHECK(drmSyncobjCreate(drm, 0, &source) == 0);
    CHECK(drmSyncobjCreate(drm, 0, &destination) == 0);
    int exported = 123;
    CHECK(tu_kgsl_drm_export_wait(drm, source, point, &exported) != 0 && exported == -1);
    CHECK(tu_kgsl_drm_import_signal(drm, source, point, fence) == 0);
    CHECK(tu_kgsl_drm_export_wait(drm, source, point, &exported) == 0 && exported >= 0);
    check_pending(exported);
    CHECK(tu_kgsl_drm_import_signal(drm, destination, point, exported) == 0);
    int back;
    CHECK(tu_kgsl_drm_export_wait(drm, destination, point, &back) == 0);
    check_pending(back);
    int object_fd;
    CHECK(drmSyncobjHandleToFD(drm, destination, &object_fd) == 0);
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(sockets[0]); close(drm); close(software);
        close(fence); close(exported); close(back);
        int peer = open(render_node, O_RDWR | O_CLOEXEC);
        CHECK(peer >= 0);
        uint32_t imported;
        CHECK(drmSyncobjFDToHandle(peer, object_fd, &imported) == 0);
        close(object_fd);
        errno = 0;
        CHECK(drmSyncobjTimelineWait(peer, &imported, &point, 1, 0,
              DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) != 0 && errno == ETIME);
        CHECK(write(sockets[1], "r", 1) == 1);
        CHECK(drmSyncobjTimelineWait(peer, &imported, &point, 1, deadline(),
              DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) == 0);
        CHECK(drmSyncobjDestroy(peer, imported) == 0);
        close(peer); close(sockets[1]);
        _exit(0);
    }
    close(sockets[1]); close(object_fd);
    char ready;
    CHECK(read(sockets[0], &ready, 1) == 1 && ready == 'r');
    check_pending(back);
    uint32_t increment = 1;
    CHECK(ioctl(software, SW_SYNC_IOC_INC, &increment) == 0);
    check_ready(fence); check_ready(exported); check_ready(back);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(drmSyncobjDestroy(drm, source) == 0);
    CHECK(drmSyncobjDestroy(drm, destination) == 0);
    close(sockets[0]); close(fence); close(exported); close(back); close(software);
    CHECK(fd_count() == start_fds);
}

static void merged_pending(int drm)
{
    unsigned start_fds = fd_count();
    int first = open("/sys/kernel/debug/sync/sw_sync", O_RDWR | O_CLOEXEC);
    int second = open("/sys/kernel/debug/sync/sw_sync", O_RDWR | O_CLOEXEC);
    CHECK(first >= 0 && second >= 0);
    int a = create_fence(first, 1), b = create_fence(second, 1);
    struct sync_merge_data merge = {.name = "kgsl-merged-work", .fd2 = b, .fence = -1};
    CHECK(ioctl(a, SYNC_IOC_MERGE, &merge) == 0 && merge.fence >= 0);
    uint32_t object;
    CHECK(drmSyncobjCreate(drm, 0, &object) == 0);
    CHECK(tu_kgsl_drm_import_signal(drm, object, 23, merge.fence) == 0);
    close(a); close(b); close(merge.fence);
    int exported;
    CHECK(tu_kgsl_drm_export_wait(drm, object, 23, &exported) == 0);
    check_pending(exported);
    uint32_t increment = 1;
    CHECK(ioctl(first, SW_SYNC_IOC_INC, &increment) == 0);
    check_pending(exported);
    CHECK(ioctl(second, SW_SYNC_IOC_INC, &increment) == 0);
    check_ready(exported);
    CHECK(drmSyncobjDestroy(drm, object) == 0);
    close(exported); close(first); close(second);
    CHECK(fd_count() == start_fds);
}

static void future_pending(const char *render_node, int drm)
{
    unsigned start_fds = fd_count();
    int software = open("/sys/kernel/debug/sync/sw_sync", O_RDWR | O_CLOEXEC);
    CHECK(software >= 0);
    int fence = create_fence(software, 1);
    uint32_t object;
    CHECK(drmSyncobjCreate(drm, 0, &object) == 0);
    int object_fd;
    CHECK(drmSyncobjHandleToFD(drm, object, &object_fd) == 0);
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(sockets[0]); close(drm); close(software); close(fence);
        int peer = open(render_node, O_RDWR | O_CLOEXEC);
        CHECK(peer >= 0);
        uint32_t imported;
        CHECK(drmSyncobjFDToHandle(peer, object_fd, &imported) == 0);
        close(object_fd);
        uint64_t point = 42;
        CHECK(write(sockets[1], "r", 1) == 1);
        CHECK(drmSyncobjTimelineWait(peer, &imported, &point, 1, deadline(),
              DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
              DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, NULL) == 0);
        int exported;
        CHECK(tu_kgsl_drm_export_wait(peer, imported, point, &exported) == 0);
        check_pending(exported);
        CHECK(write(sockets[1], "p", 1) == 1);
        check_ready(exported);
        CHECK(drmSyncobjDestroy(peer, imported) == 0);
        close(exported); close(peer); close(sockets[1]);
        _exit(0);
    }
    close(sockets[1]); close(object_fd);
    char ready;
    CHECK(read(sockets[0], &ready, 1) == 1 && ready == 'r');
    struct pollfd available = {.fd = sockets[0], .events = POLLIN};
    CHECK(poll(&available, 1, 100) == 0);
    CHECK(tu_kgsl_drm_import_signal(drm, object, 42, fence) == 0);
    CHECK(read(sockets[0], &ready, 1) == 1 && ready == 'p');
    check_pending(fence);
    uint32_t increment = 1;
    CHECK(ioctl(software, SW_SYNC_IOC_INC, &increment) == 0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(drmSyncobjDestroy(drm, object) == 0);
    close(sockets[0]); close(fence); close(software);
    CHECK(fd_count() == start_fds);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    alarm(20);
    int drm = open(argv[1], O_RDWR | O_CLOEXEC);
    CHECK(drm >= 0);
    uint64_t cap = 0;
    CHECK(drmGetCap(drm, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap == 1);
    roundtrip(argv[1], drm, 0);
    roundtrip(argv[1], drm, 17);
    merged_pending(drm);
    future_pending(argv[1], drm);
    uint32_t object;
    CHECK(drmSyncobjCreate(drm, 0, &object) == 0);
    unsigned start_fds = fd_count();
    for (uint64_t point = 1; point <= 128; ++point) {
        CHECK(tu_kgsl_drm_import_signal(drm, object, point, -1) == 0);
        int fd;
        CHECK(tu_kgsl_drm_export_wait(drm, object, point, &fd) == 0);
        check_ready(fd);
        CHECK(tu_kgsl_drm_import_signal(drm, UINT32_MAX, 0, fd) != 0);
        check_ready(fd);
        close(fd);
    }
    CHECK(fd_count() == start_fds);
    int fd;
    CHECK(tu_kgsl_drm_export_wait(drm, UINT32_MAX, 0, &fd) != 0 && fd == -1);
    CHECK(tu_kgsl_drm_import_signal(drm, object, 129, 2147483000) != 0);
    uint64_t point = 129;
    CHECK(drmSyncobjTimelineWait(drm, &object, &point, 1, 0,
          DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) != 0);
    CHECK(fd_count() == start_fds);
    CHECK(drmSyncobjDestroy(drm, object) == 0);
    close(drm);
    puts("PASS: pending sync_file -> DRM binary/timeline -> sync_file; cross-process opaque FD; no early completion; merged work waits for both fences after source FDs close; future timeline point becomes available before completion; real fence completion; signaled empty work; 128 cleanup cycles; invalid handles/FDs rejected. Kernel software fences, not KGSL GPU execution.");
    return 0;
}
