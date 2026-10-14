/* SPDX-License-Identifier: MIT */
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
   std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(2); \
} } while (0)

enum VkResult { VK_SUCCESS = 0, VK_ERROR_OUT_OF_HOST_MEMORY = -1,
                VK_ERROR_OUT_OF_DEVICE_MEMORY = -2, VK_ERROR_FEATURE_NOT_PRESENT = -8 };
enum tu_bo_alloc_flags { TU_BO_ALLOC_NO_FLAGS = 0, TU_BO_ALLOC_IMPLICIT_SYNC = 1 };
struct tu_physical_device { int local_fd; };
struct tu_device { tu_physical_device *physical_device; };
struct tu_bo {
   uint32_t gem_handle;
   uint64_t size, iova;
   const char *name;
   unsigned refcnt;
   int shared_fd;
   uint64_t unique_id;
};
struct kgsl_gpuobj_import_dma_buf { int fd; };
struct kgsl_gpuobj_import { uintptr_t priv; uint64_t priv_len, flags; unsigned type, id; };
struct kgsl_gpuobj_info { unsigned id; uint64_t size, gpuaddr; };
struct kgsl_gpuobj_free { unsigned id; };
enum { IOCTL_KGSL_GPUOBJ_IMPORT, IOCTL_KGSL_GPUOBJ_INFO, IOCTL_KGSL_GPUOBJ_FREE,
       KGSL_USER_MEM_TYPE_DMABUF };
enum class Fault { None, Import, Info, Duplicate };
static Fault fault;
static unsigned imports, queries, releases, lookups, debug_adds, dumps, next_id = 1;
static int failure_errno;
static bool release_fails;
static tu_bo entry;
static std::map<unsigned, int> objects;
static char diagnostic[512];

static VkResult vk_errorf(tu_device *, VkResult result, const char *format, ...)
{
   va_list args;
   va_start(args, format);
   vsnprintf(diagnostic, sizeof(diagnostic), format, args);
   va_end(args);
   return result;
}

static int safe_ioctl(int fd, unsigned request, void *arg)
{
   CHECK(fd == 91);
   switch (request) {
   case IOCTL_KGSL_GPUOBJ_IMPORT: {
      ++imports;
      auto *req = static_cast<kgsl_gpuobj_import *>(arg);
      CHECK(req->type == KGSL_USER_MEM_TYPE_DMABUF && req->flags == 0);
      CHECK(req->priv_len == sizeof(kgsl_gpuobj_import_dma_buf));
      if (fault == Fault::Import) { errno = failure_errno; return -1; }
      int borrowed = reinterpret_cast<kgsl_gpuobj_import_dma_buf *>(req->priv)->fd;
      int held = fcntl(borrowed, F_DUPFD_CLOEXEC, 0);
      CHECK(held >= 0);
      req->id = next_id++;
      objects.emplace(req->id, held);
      return 0;
   }
   case IOCTL_KGSL_GPUOBJ_INFO: {
      ++queries;
      auto *req = static_cast<kgsl_gpuobj_info *>(arg);
      CHECK(objects.count(req->id));
      if (fault == Fault::Info) { errno = failure_errno; return -1; }
      req->size = 65536;
      req->gpuaddr = UINT64_C(0x123450000);
      return 0;
   }
   case IOCTL_KGSL_GPUOBJ_FREE: {
      ++releases;
      unsigned id = static_cast<kgsl_gpuobj_free *>(arg)->id;
      CHECK(objects.count(id));
      if (release_fails) { errno = EBUSY; return -1; }
      CHECK(close(objects.at(id)) == 0);
      objects.erase(id);
      errno = ENOTTY;
      return 0;
   }
   default: CHECK(false); return -1;
   }
}

static int os_dupfd_cloexec(int fd)
{
   if (fault == Fault::Duplicate) { errno = failure_errno; return -1; }
   return fcntl(fd, F_DUPFD_CLOEXEC, 0);
}
static tu_bo *tu_device_lookup_bo(tu_device *, unsigned id)
{
   ++lookups;
   CHECK(objects.count(id) && entry.gem_handle == 0);
   return &entry;
}
static const char *tu_debug_bos_add(tu_device *, uint64_t size, const char *name)
{
   CHECK(size == 65536 && !strcmp(name, "dmabuf"));
   ++debug_adds;
   return name;
}
static void tu_dump_bo_init(tu_device *, tu_bo *) { ++dumps; }
#include "kgsl-import-under-test.h"

int main()
{
   unsigned violations = 0, cases = 0;
   FILE *backing = tmpfile(); CHECK(backing);
   int caller = fileno(backing); CHECK(ftruncate(caller, 65536) == 0);
   struct stat original; CHECK(fstat(caller, &original) == 0);
   tu_physical_device physical{91}; tu_device dev{&physical};
   tu_bo sentinel{};
   for (unsigned repeat = 0; repeat < 128; ++repeat) {
      for (Fault selected : {Fault::None, Fault::Import, Fault::Info, Fault::Duplicate}) {
         for (int error : {EIO, ENOMEM, EMFILE}) {
            for (bool cleanup_error : {false, true}) {
               if (cleanup_error && (selected == Fault::None || selected == Fault::Import)) continue;
               ++cases;
               fault = selected; failure_errno = error; release_fails = cleanup_error;
               imports = queries = releases = lookups = debug_adds = dumps = 0;
               entry = {}; diagnostic[0] = '\0';
               tu_bo *out = &sentinel;
               VkResult result = kgsl_bo_init_dmabuf(&dev, &out, 65536, TU_BO_ALLOC_NO_FLAGS, caller);
               bool valid;
               if (fault == Fault::None) {
                  valid = result == VK_SUCCESS && out == &entry && imports == 1 && queries == 1 &&
                          releases == 0 && lookups == 1 && debug_adds == 1 && dumps == 1 &&
                          entry.size == 65536 && entry.iova == UINT64_C(0x123450000) &&
                          entry.refcnt == 1 && entry.shared_fd >= 0 && entry.shared_fd != caller &&
                          (fcntl(entry.shared_fd, F_GETFD) & FD_CLOEXEC) &&
                          entry.unique_id == (uint64_t(original.st_ino) | (UINT64_C(1) << 63));
                  CHECK(valid);
                  int exported = kgsl_bo_export_dmabuf(&dev, out);
                  CHECK(exported >= 0 && exported != caller && exported != entry.shared_fd);
                  CHECK(fcntl(exported, F_GETFD) & FD_CLOEXEC);
                  CHECK(close(exported) == 0);
               } else {
                  bool imported = fault != Fault::Import;
                  VkResult expected = fault == Fault::Duplicate ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_OUT_OF_DEVICE_MEMORY;
                  valid = result == expected && out == &sentinel && imports == 1 &&
                          queries == unsigned(imported) && releases == unsigned(imported) &&
                          lookups == 0 && debug_adds == 0 && dumps == 0 && entry.gem_handle == 0 &&
                          objects.size() == unsigned(imported && cleanup_error) &&
                          strstr(diagnostic, strerror(error));
                  if (!valid) ++violations;
               }
               CHECK(fcntl(caller, F_GETFD) >= 0);
               if (entry.gem_handle && entry.shared_fd >= 0) CHECK(close(entry.shared_fd) == 0);
               for (auto &object : objects) CHECK(close(object.second) == 0);
               objects.clear();
            }
         }
      }
   }
   fault = Fault::None; imports = 0; tu_bo *out = &sentinel;
   CHECK(kgsl_bo_init_dmabuf(&dev, &out, 65536, TU_BO_ALLOC_IMPLICIT_SYNC, caller) == VK_ERROR_FEATURE_NOT_PRESENT);
   CHECK(imports == 0 && out == &sentinel && objects.empty());
   CHECK(fclose(backing) == 0);
   std::printf("KGSL import: %u cases, %u ownership failures; modeled ioctls, real descriptors\n", cases, violations);
   return violations ? 1 : 0;
}
