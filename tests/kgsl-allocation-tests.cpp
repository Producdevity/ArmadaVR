/* SPDX-License-Identifier: MIT */
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#define __user
#include "msm_kgsl.h"

#define CHECK(expr) do { if (!(expr)) { \
   std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(2); \
} } while (0)

enum VkResult { VK_SUCCESS = 0, VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
                VK_ERROR_FEATURE_NOT_PRESENT = -8, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS = -1000257000 };
using VkMemoryPropertyFlags = unsigned;
enum { VK_MEMORY_PROPERTY_HOST_CACHED_BIT = 1, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 2 };
enum tu_bo_alloc_flags { TU_BO_ALLOC_NO_FLAGS = 0, TU_BO_ALLOC_IMPLICIT_SYNC = 1,
   TU_BO_ALLOC_SHAREABLE = 2, TU_BO_ALLOC_REPLAYABLE = 4,
   TU_BO_ALLOC_GPU_READ_ONLY = 8, TU_BO_ALLOC_INTERNAL_RESOURCE = 16 };
enum tu_sparse_vma_flags { TU_SPARSE_VMA_REPLAYABLE = 1, TU_SPARSE_VMA_MAP_ZERO = 2 };
enum { TU_KGSL_DMA_TYPE_DMAHEAP, TU_KGSL_DMA_TYPE_ION, TU_KGSL_DMA_TYPE_ION_LEGACY };
struct vk_object_base {};
struct tu_physical_device { int local_fd, kgsl_dma_type; };
struct tu_device { tu_physical_device *physical_device; };
struct tu_bo {
   uint32_t gem_handle;
   uint64_t size, iova;
   const char *name;
   unsigned refcnt;
   int shared_fd;
   vk_object_base *base;
   void *map;
   bool never_unmap;
   unsigned dump_bo_list_idx;
};
struct tu_sparse_vma { struct { tu_bo *virtual_bo; } kgsl; };
enum class Fault { None, Alloc, Info, Map, Bind, Address };
static Fault fault;
static int failure_errno;
static bool release_fails;
static unsigned allocations, queries, releases, binds, debug_count, invalid_dumps;
static unsigned rmv_allocs, rmv_frees, objects, attempts;
static tu_bo entry;
static tu_bo *dump_entry;
static std::map<void *, size_t> mappings;
static char diagnostic[512];
static constexpr uint64_t size = 65536;

static VkResult vk_errorf(tu_device *, VkResult result, const char *format, ...)
{
   va_list args; va_start(args, format);
   vsnprintf(diagnostic, sizeof(diagnostic), format, args); va_end(args);
   return result;
}
static int safe_ioctl(int fd, unsigned long request, void *arg)
{
   CHECK(fd >= 0);
   if (request == IOCTL_KGSL_GPUOBJ_ALLOC) {
      ++allocations;
      auto *req = static_cast<kgsl_gpuobj_alloc *>(arg);
      CHECK(req->size == size && !objects);
      if (fault == Fault::Alloc) { errno = failure_errno; return -1; }
      req->id = 17; req->mmapsize = size; objects = 1; return 0;
   }
   if (request == IOCTL_KGSL_GPUOBJ_INFO) {
      ++queries;
      auto *req = static_cast<kgsl_gpuobj_info *>(arg);
      CHECK(req->id == 17 && objects == 1);
      if (fault == Fault::Info) { errno = failure_errno; return -1; }
      req->gpuaddr = UINT64_C(0x123450000); return 0;
   }
   if (request == IOCTL_KGSL_GPUOBJ_FREE) {
      ++releases;
      CHECK(static_cast<kgsl_gpuobj_free *>(arg)->id == 17 && objects == 1);
      if (release_fails) { errno = EBUSY; return -1; }
      objects = 0; errno = ENOTTY; return 0;
   }
   if (request == IOCTL_KGSL_GPUMEM_BIND_RANGES) {
      ++binds;
      auto *req = static_cast<kgsl_gpumem_bind_ranges *>(arg);
      auto *range = reinterpret_cast<kgsl_gpumem_bind_range *>(req->ranges);
      CHECK(req->id == 91 && req->ranges_nents == 1 && req->ranges_size == sizeof(*range));
      CHECK(range->child_id == 17 && range->length == size && range->child_offset == 0);
      if (fault == Fault::Bind) { errno = failure_errno; return -1; }
      return 0;
   }
   CHECK(false); return -1;
}
static void *test_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
   ++attempts;
   CHECK(length == size && flags == MAP_SHARED && offset == (17 << 12));
   if (fault == Fault::Map) { errno = failure_errno; return MAP_FAILED; }
   void *result = mmap(address, length, prot, flags, fd, offset);
   CHECK(result != MAP_FAILED && !mappings.count(result));
   mappings.emplace(result, length); return result;
}
static int test_munmap(void *address, size_t length)
{
   CHECK(mappings.count(address) && mappings.at(address) == length);
   CHECK(munmap(address, length) == 0); mappings.erase(address); errno = ENOTTY; return 0;
}
static tu_bo *tu_device_lookup_bo(tu_device *, unsigned id)
{
   CHECK(id == 17 && objects == 1 && !entry.gem_handle); return &entry;
}
static const char *tu_debug_bos_add(tu_device *, uint64_t length, const char *name)
{
   CHECK(length == size && name && !debug_count); ++debug_count; return name;
}
static void tu_debug_bos_del(tu_device *, tu_bo *bo)
{
   if (bo->name) { CHECK(debug_count == 1); --debug_count; }
}
static void tu_dump_bo_init(tu_device *, tu_bo *bo)
{
   CHECK(!dump_entry); dump_entry = bo; bo->dump_bo_list_idx = 0;
}
static void tu_dump_bo_del(tu_device *, tu_bo *bo)
{
   if (bo->dump_bo_list_idx != ~0u) {
      if (dump_entry != bo) ++invalid_dumps;
      else dump_entry = nullptr;
   }
}
static bool p_atomic_dec_zero(unsigned *ref) { CHECK(*ref); return --*ref == 0; }
static void rmv_bo_allocate(tu_bo *) { ++rmv_allocs; }
static void rmv_bo_destroy(tu_bo *) { ++rmv_frees; }
static void rmv_bo_unmap(tu_bo *) {}
static void rmv_internal_resource_create(tu_bo *) {}
static void rmv_resource_name(tu_bo *, const char *) {}
#define TU_RMV(event, dev, ...) rmv_##event(__VA_ARGS__)
static VkResult unsupported(tu_device *, tu_bo **, uint64_t, tu_bo_alloc_flags) { CHECK(false); return VK_SUCCESS; }
#define bo_init_new_dmaheap unsupported
#define bo_init_new_ion unsupported
#define bo_init_new_ion_legacy unsupported
#define mmap test_mmap
#define munmap test_munmap
static void kgsl_bo_finish(tu_device *, tu_bo *);
#include "kgsl-allocation-under-test.h"
#undef mmap
#undef munmap

enum class Mode { Native, Lazy, Replay, Fixed, Sparse, SparseReplay, SparseFixed };
int main()
{
   FILE *backing = tmpfile(); CHECK(backing);
   CHECK(ftruncate(fileno(backing), 1024 * 1024) == 0);
   tu_physical_device physical{fileno(backing), TU_KGSL_DMA_TYPE_DMAHEAP}; tu_device dev{&physical};
   unsigned cases = 0, failures = 0;
   for (unsigned repeat = 0; repeat < 64; ++repeat) {
      for (Mode mode : {Mode::Native, Mode::Lazy, Mode::Replay, Mode::Fixed,
                        Mode::Sparse, Mode::SparseReplay, Mode::SparseFixed}) {
         bool sparse = mode == Mode::Sparse || mode == Mode::SparseReplay || mode == Mode::SparseFixed;
         bool replay = mode == Mode::Replay || mode == Mode::Fixed || mode == Mode::SparseReplay || mode == Mode::SparseFixed;
         bool fixed = mode == Mode::Fixed || mode == Mode::SparseFixed;
         bool lazy = mode == Mode::Lazy;
         for (Fault selected : {Fault::None, Fault::Alloc, Fault::Info, Fault::Map, Fault::Bind, Fault::Address}) {
            if ((selected == Fault::Info && (replay || lazy)) ||
                (selected == Fault::Bind && !lazy) || (selected == Fault::Map && !replay) ||
                (selected == Fault::Address && !fixed)) continue;
            for (int error : {ENOMEM, EIO, EACCES}) {
               for (bool cleanup_error : {false, true}) {
                  if (cleanup_error && (selected == Fault::None || selected == Fault::Alloc)) continue;
                  ++cases; fault = selected; failure_errno = error; release_fails = cleanup_error;
                  allocations = queries = releases = binds = debug_count = invalid_dumps = 0;
                  rmv_allocs = rmv_frees = objects = attempts = 0; entry = {}; dump_entry = nullptr;
                  diagnostic[0] = '\0'; CHECK(mappings.empty());
                  void *hint = nullptr;
                  if (fixed) {
                     hint = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                     CHECK(hint != MAP_FAILED);
                     if (selected != Fault::Address) CHECK(munmap(hint, size) == 0);
                     else memset(hint, 0x5a, size);
                  }
                  tu_bo sentinel{}, parent{}; parent.gem_handle = 91; parent.size = size;
                  tu_sparse_vma lazy_vma{{&parent}}, out_vma{{&sentinel}};
                  tu_bo *out = &sentinel; uint64_t iova = 0xbeef;
                  VkResult result = sparse ?
                     kgsl_sparse_vma_init(&dev, nullptr, &out_vma, &iova,
                        static_cast<tu_sparse_vma_flags>(replay ? TU_SPARSE_VMA_REPLAYABLE : TU_SPARSE_VMA_MAP_ZERO), size, uintptr_t(hint)) :
                     kgsl_bo_init(&dev, nullptr, &out, size, uintptr_t(hint), 0,
                        replay ? TU_BO_ALLOC_REPLAYABLE : TU_BO_ALLOC_NO_FLAGS, lazy ? &lazy_vma : nullptr, "test");
                  bool valid = true;
                  if (selected == Fault::None) {
                     CHECK(result == VK_SUCCESS && objects == 1 && releases == 0);
                     CHECK(sparse ? out_vma.kgsl.virtual_bo == &entry : out == &entry);
                     CHECK(entry.size == size && entry.refcnt == 1 && entry.shared_fd == -1);
                     if (replay) {
                        CHECK(entry.map && entry.never_unmap && entry.iova == uintptr_t(entry.map));
                        if (fixed) CHECK(entry.map == hint);
                        memset(entry.map, 0xa5, size);
                     }
                     int shared = -1;
                     if (!sparse) {
                        shared = fcntl(fileno(backing), F_DUPFD_CLOEXEC, 0);
                        CHECK(shared >= 0); entry.shared_fd = shared;
                     }
                     if (sparse) kgsl_sparse_vma_finish(&dev, &out_vma);
                     else {
                        ++entry.refcnt; kgsl_bo_finish(&dev, out);
                        CHECK(objects == 1 && entry.refcnt == 1 && releases == 0);
                        kgsl_bo_finish(&dev, out);
                     }
                     if (shared >= 0) CHECK(fcntl(shared, F_GETFD) == -1 && errno == EBADF);
                     CHECK(fcntl(fileno(backing), F_GETFD) >= 0);
                     valid = objects == 0 && releases == 1 && !entry.gem_handle && mappings.empty() &&
                             debug_count == 0 && !dump_entry && invalid_dumps == 0 &&
                             rmv_allocs == unsigned(!sparse) && rmv_frees == rmv_allocs;
                  } else {
                     VkResult expected = selected == Fault::Address ? VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
                     bool allocated = selected != Fault::Alloc;
                     valid = result == expected && out == &sentinel && out_vma.kgsl.virtual_bo == &sentinel && iova == 0xbeef &&
                             allocations == 1 && releases == unsigned(allocated) &&
                             objects == unsigned(allocated && cleanup_error) && !entry.gem_handle &&
                             mappings.empty() && debug_count == 0 && !dump_entry && invalid_dumps == 0 &&
                             rmv_allocs == 0 && rmv_frees == 0 &&
                             (selected == Fault::Address || strstr(diagnostic, strerror(error)));
                  }
                  if (!valid) {
                     ++failures;
                     if (repeat == 0 && error == ENOMEM && !cleanup_error)
                        std::printf("allocation regression: mode=%d fault=%d objects=%u mappings=%zu invalid_dump=%u trace_frees=%u\n",
                                    int(mode), int(selected), objects, mappings.size(), invalid_dumps, rmv_frees);
                  }
                  for (auto &mapping : mappings) CHECK(munmap(mapping.first, mapping.second) == 0);
                  mappings.clear();
                  if (selected == Fault::Address) {
                     CHECK(static_cast<unsigned char *>(hint)[0] == 0x5a && static_cast<unsigned char *>(hint)[size-1] == 0x5a);
                     CHECK(munmap(hint, size) == 0);
                  }
               }
            }
         }
      }
   }
   CHECK(fclose(backing) == 0);
   std::printf("KGSL allocation: %u cases, %u lifetime failures; modeled GPU ioctls, real mmap/munmap\n", cases, failures);
   return failures ? 1 : 0;
}
