/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include "tu_kgsl_display.h"

static void finish(int failed)
{
   puts(failed ? "KGSL_DISPLAY_FAIL" : "KGSL_DISPLAY_ALL_PASS");
   fflush(NULL);
   sync();
   reboot(RB_POWER_OFF);
   _exit(failed);
}
#define CHECK(expr) do { if (!(expr)) { \
   fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #expr, errno); finish(1); \
} } while (0)
static void expired(int signo) { (void)signo; finish(1); }
static unsigned fd_count(void)
{
   DIR *dir = opendir("/proc/self/fd"); CHECK(dir);
   unsigned n = 0; while (readdir(dir)) ++n;
   CHECK(closedir(dir) == 0); return n;
}

int main(void)
{
   if (getpid() != 1) {
      fputs("Only run as PID 1 in the diskless QEMU test.\n", stderr);
      return 2;
   }
   CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
   char cmdline[4096];
   FILE *cmd = fopen("/proc/cmdline", "r"); CHECK(cmd);
   CHECK(fgets(cmdline, sizeof(cmdline), cmd) && strstr(cmdline, "armada.kgsl_display_test=1"));
   CHECK(fclose(cmd) == 0);
   signal(SIGALRM, expired); alarm(20);
   CHECK(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0);
   CHECK(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0);
   const char *primary = "/dev/dri/card0", *render = "/dev/dri/renderD128";
   for (unsigned i = 0; i < 100 && access(primary, F_OK); ++i) usleep(10000);
   unsigned initial = fd_count();
   int64_t device_major = -1, device_minor = -1;
   CHECK(tu_kgsl_display_open(NULL, &device_major, &device_minor) == -1 && errno == EINVAL);
   CHECK(tu_kgsl_display_open("/dev/null", &device_major, &device_minor) == -1 && errno == ENODEV);
   int master = tu_kgsl_display_open(primary, &device_major, &device_minor);
   CHECK(master >= 0 && drmIsMaster(master));
   drmVersionPtr version = drmGetVersion(master);
   CHECK(version && strcmp(version->name, "virtio_gpu") == 0);
   drmFreeVersion(version);
   CHECK(fcntl(master, F_GETFD) & FD_CLOEXEC);
   CHECK(tu_kgsl_display_matches_fd(master, device_major, device_minor));
   int render_fd = open(render, O_RDWR | O_CLOEXEC); CHECK(render_fd >= 0);
   CHECK(drmGetNodeTypeFromFd(render_fd) == DRM_NODE_RENDER);
   CHECK(!tu_kgsl_display_matches_fd(render_fd, device_major, device_minor));
   int64_t untouched_major = -5, untouched_minor = -7;
   CHECK(tu_kgsl_display_open(render, &untouched_major, &untouched_minor) == -1 && errno == ENODEV);
   CHECK(untouched_major == -5 && untouched_minor == -7);
   puts("KGSL_DISPLAY_PRIMARY_AND_RENDER_PASS");

   unsigned held = fd_count();
   for (unsigned i = 0; i < 128; ++i) {
      CHECK(tu_kgsl_display_open(primary, &untouched_major, &untouched_minor) == -1 && errno == EACCES);
      CHECK(untouched_major == -5 && untouched_minor == -7);
      CHECK(fd_count() == held && drmIsMaster(master));
   }
   puts("KGSL_DISPLAY_MASTER_CONTENTION_PASS");
   drmModeRes *res = drmModeGetResources(master); CHECK(res);
   drmModePlaneRes *planes = drmModeGetPlaneResources(master); CHECK(planes);
   CHECK(res->count_connectors > 0 && res->count_crtcs > 0 && planes->count_planes > 0);
   uint32_t objects[] = {res->connectors[0], res->crtcs[0], planes->planes[0]}, lessee;
   int lease = drmModeCreateLease(master, objects, 3, O_CLOEXEC, &lessee); CHECK(lease >= 0);
   CHECK(tu_kgsl_display_matches_fd(lease, device_major, device_minor));
   drmModeRes *leased = drmModeGetResources(lease); CHECK(leased);
   CHECK(leased->count_connectors == 1 && leased->count_crtcs == 1);
   drmModeFreeResources(leased);
   CHECK(drmModeRevokeLease(master, lessee) == 0);
   CHECK(close(lease) == 0);
   drmModeFreeResources(res); drmModeFreePlaneResources(planes);
   puts("KGSL_DISPLAY_LEASE_IDENTITY_PASS");
   CHECK(close(render_fd) == 0 && close(master) == 0 && fd_count() == initial);
   for (unsigned i = 0; i < 128; ++i) {
      master = tu_kgsl_display_open(primary, &device_major, &device_minor); CHECK(master >= 0);
      CHECK(drmIsMaster(master));
      CHECK(drmDropMaster(master) == 0 && !drmIsMaster(master));
      CHECK(drmSetMaster(master) == 0 && drmIsMaster(master));
      CHECK(close(master) == 0 && fd_count() == initial);
   }
   puts("KGSL_DISPLAY_REACQUIRE_CLEANUP_PASS");
   puts("Virtual DRM descriptor ownership only; no KGSL rendering or panel execution.");
   finish(0);
}
