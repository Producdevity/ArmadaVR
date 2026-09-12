// SPDX-License-Identifier: MIT
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define CHECK(expr) do { if (!(expr)) { \
   fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #expr, errno); abort(); \
} } while (0)

enum failure {
   NONE, STAT, NODE, PRIME_QUERY, NO_IMPORT, UNIVERSAL, ATOMIC, NOT_MASTER,
   RESOURCES, NO_CONNECTORS, NO_CRTCS, PLANES, NO_PLANES
};
static failure fault;
static int resource_count, plane_count, close_count;
static unsigned cap_calls;

static int test_stat(int fd, struct stat *st)
{
   if (fault == STAT) { errno = EIO; return -1; }
   return fstat(fd, st);
}
static int test_node(int) { return fault == NODE ? DRM_NODE_RENDER : DRM_NODE_PRIMARY; }
static int test_cap(int, uint64_t cap, uint64_t *value)
{
   CHECK(cap == DRM_CAP_PRIME);
   if (fault == PRIME_QUERY) { errno = EIO; return -1; }
   *value = fault == NO_IMPORT ? DRM_PRIME_CAP_EXPORT : DRM_PRIME_CAP_IMPORT;
   return 0;
}
static int test_client_cap(int, uint64_t cap, uint64_t value)
{
   CHECK(value == 1);
   CHECK(cap == (cap_calls++ ? DRM_CLIENT_CAP_ATOMIC : DRM_CLIENT_CAP_UNIVERSAL_PLANES));
   if ((cap == DRM_CLIENT_CAP_ATOMIC && fault == ATOMIC) ||
       (cap == DRM_CLIENT_CAP_UNIVERSAL_PLANES && fault == UNIVERSAL)) {
      errno = EOPNOTSUPP; return -1;
   }
   return 0;
}
static int test_master(int) { return fault != NOT_MASTER; }
static drmModeRes *test_resources(int)
{
   if (fault == RESOURCES) { errno = ENOMEM; return nullptr; }
   auto *res = static_cast<drmModeRes *>(calloc(1, sizeof(drmModeRes)));
   CHECK(res); ++resource_count;
   res->count_connectors = fault != NO_CONNECTORS;
   res->count_crtcs = fault != NO_CRTCS;
   return res;
}
static void test_free_resources(drmModeRes *res) { --resource_count; free(res); }
static drmModePlaneRes *test_planes(int)
{
   if (fault == PLANES) { errno = ENOMEM; return nullptr; }
   auto *res = static_cast<drmModePlaneRes *>(calloc(1, sizeof(drmModePlaneRes)));
   CHECK(res); ++plane_count;
   res->count_planes = fault != NO_PLANES;
   return res;
}
static void test_free_planes(drmModePlaneRes *res) { --plane_count; free(res); }
static int test_close(int fd)
{
   CHECK(close(fd) == 0); ++close_count;
   errno = EBADF; // Detect a cleanup call overwriting the original failure.
   return 0;
}
#define fstat test_stat
#define close test_close
#define drmGetNodeTypeFromFd test_node
#define drmGetCap test_cap
#define drmSetClientCap test_client_cap
#define drmIsMaster test_master
#define drmModeGetResources test_resources
#define drmModeFreeResources test_free_resources
#define drmModeGetPlaneResources test_planes
#define drmModeFreePlaneResources test_free_planes
#include "tu_kgsl_display.h"
#undef fstat
#undef close

static unsigned fd_count()
{
   DIR *dir = opendir("/proc/self/fd"); CHECK(dir);
   unsigned n = 0; while (readdir(dir)) ++n;
   CHECK(closedir(dir) == 0); return n;
}

int main()
{
   alarm(15);
   unsigned initial = fd_count();
   int64_t device_major = -5, device_minor = -7;
   CHECK(tu_kgsl_display_open(nullptr, &device_major, &device_minor) == -1 && errno == EINVAL);
   CHECK(tu_kgsl_display_open("", &device_major, &device_minor) == -1 && errno == EINVAL);
   CHECK(tu_kgsl_display_open("/absent-display", &device_major, &device_minor) == -1 && errno == ENOENT);
   CHECK(close_count == 0);
   CHECK(tu_kgsl_display_open("/proc/self/status", &device_major, &device_minor) == -1 && errno == ENODEV);
   CHECK(close_count == 1);
   for (int i = STAT; i <= NO_PLANES; ++i) {
      fault = static_cast<failure>(i); cap_calls = 0;
      int previous = close_count;
      CHECK(tu_kgsl_display_open("/dev/null", &device_major, &device_minor) == -1);
      int expected = (fault == STAT || fault == PRIME_QUERY) ? EIO :
         (fault == UNIVERSAL || fault == ATOMIC || fault == NO_IMPORT) ? EOPNOTSUPP :
         fault == NOT_MASTER ? EACCES :
         (fault == RESOURCES || fault == PLANES) ? ENOMEM : ENODEV;
      CHECK(errno == expected);
      CHECK(device_major == -5 && device_minor == -7);
      CHECK(close_count == previous + 1 && resource_count == 0 && plane_count == 0);
      CHECK(fd_count() == initial);
   }
   fault = NONE;
   for (unsigned i = 0; i < 1024; ++i) {
      cap_calls = 0;
      int fd = tu_kgsl_display_open("/dev/null", &device_major, &device_minor);
      CHECK(fd >= 0 && cap_calls == 2);
      CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
      struct stat st; CHECK(fstat(fd, &st) == 0);
      CHECK(device_major == major(st.st_rdev) && device_minor == minor(st.st_rdev));
      CHECK(tu_kgsl_display_matches_fd(fd, device_major, device_minor));
      int other = open("/dev/zero", O_RDWR | O_CLOEXEC); CHECK(other >= 0);
      CHECK(!tu_kgsl_display_matches_fd(other, device_major, device_minor));
      CHECK(!tu_kgsl_display_matches_fd(-1, device_major, device_minor));
      CHECK(close(other) == 0 && close(fd) == 0);
      CHECK(resource_count == 0 && plane_count == 0 && fd_count() == initial);
   }
   puts("PASS: actual display helper, 13 injected failures, preserved errno/output identity, resource/FD ownership, CLOEXEC, node matching and 1024 cleanup cycles; ASan/UBSan; modeled DRM, no GPU execution.");
}
