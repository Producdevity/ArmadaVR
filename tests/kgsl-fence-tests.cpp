#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::exit(1); \
} } while (0)

enum VkResult { VK_SUCCESS = 0, VK_ERROR_OUT_OF_HOST_MEMORY = -1, VK_ERROR_UNKNOWN = -13 };
struct vk_object_base { uint64_t unused; };
struct tu_queue { unsigned id; };
struct kgsl_syncobj;
static int kgsl_syncobj_ts_to_fd(const kgsl_syncobj *sync);

static std::map<int, unsigned> descriptors;
static int next_fd = 10, operation = 0, fail_at = 0;

static int allocate(unsigned dependencies)
{
    if (++operation == fail_at) {
        errno = EMFILE;
        return -1;
    }
    int fd = next_fd++;
    descriptors[fd] = dependencies;
    return fd;
}

static int model_dup(int fd)
{
    auto found = descriptors.find(fd);
    if (found == descriptors.end()) {
        errno = EBADF;
        return -1;
    }
    return allocate(found->second);
}

static int model_close(int fd)
{
    CHECK(descriptors.erase(fd) == 1);
    return 0;
}

static int model_merge(const char *, int left, int right)
{
    if (!descriptors.count(left) || !descriptors.count(right)) {
        errno = EBADF;
        return -1;
    }
    return allocate(descriptors.at(left) | descriptors.at(right));
}

#define close model_close
#define dup model_dup
#define os_dupfd_cloexec model_dup
#define sync_merge model_merge
#define ASSERTED [[maybe_unused]]
#define UNREACHABLE(message) std::abort()
#include "kgsl-sync-under-test.h"
#undef close
#undef dup
#undef os_dupfd_cloexec
#undef sync_merge

static int kgsl_syncobj_ts_to_fd(const kgsl_syncobj *sync)
{
    CHECK(sync->state == KGSL_SYNCOBJ_STATE_TS);
    return allocate(1u << sync->queue->id);
}

static VkResult merge(const kgsl_syncobj **waits, uint32_t count, kgsl_syncobj *out)
{
#if PATCHED_KGSL
    return kgsl_syncobj_merge(waits, count, out);
#else
    *out = kgsl_syncobj_merge(waits, count);
    return VK_SUCCESS;
#endif
}

static VkResult duplicate(const kgsl_syncobj *source, kgsl_syncobj *out)
{
#if PATCHED_KGSL
    return kgsl_syncobj_dup(source, out);
#else
    *out = kgsl_syncobj_dup(const_cast<kgsl_syncobj *>(source));
    return VK_SUCCESS;
#endif
}

static kgsl_syncobj timestamp(tu_queue *queue, uint32_t value)
{
    kgsl_syncobj result{};
    kgsl_syncobj_init(&result, false);
    result.state = KGSL_SYNCOBJ_STATE_TS;
    result.queue = queue;
    result.timestamp = value;
    return result;
}

static unsigned dependencies(const kgsl_syncobj &sync)
{
    switch (sync.state) {
    case KGSL_SYNCOBJ_STATE_SIGNALED: return 0;
    case KGSL_SYNCOBJ_STATE_TS: return 1u << sync.queue->id;
    case KGSL_SYNCOBJ_STATE_FD:
        CHECK(descriptors.count(sync.fd));
        return descriptors.at(sync.fd);
    default: CHECK(false); return 0;
    }
}

int main()
{
    tu_queue a{0}, b{1};
    kgsl_syncobj signaled{}, fd_a{}, fd_b{};
    kgsl_syncobj_init(&signaled, true);
    kgsl_syncobj_init(&fd_a, false);
    kgsl_syncobj_init(&fd_b, false);
    fd_a.state = fd_b.state = KGSL_SYNCOBJ_STATE_FD;
    fd_a.fd = allocate(4);
    fd_b.fd = allocate(8);
    const std::array<kgsl_syncobj, 6> inputs = {
        timestamp(&a, 3), timestamp(&b, 7), fd_a, fd_b,
        timestamp(&a, 5), signaled};
    unsigned cases = 0;
    for (const auto &left : inputs) {
        for (const auto &right : inputs) {
            const kgsl_syncobj *waits[] = {&left, &right};
            kgsl_syncobj out{};
            CHECK(merge(waits, 2, &out) == VK_SUCCESS);
            CHECK(dependencies(out) == (dependencies(left) | dependencies(right)));
            if (left.state == KGSL_SYNCOBJ_STATE_TS &&
                right.state == KGSL_SYNCOBJ_STATE_TS && left.queue == right.queue)
                CHECK(out.timestamp == (left.timestamp > right.timestamp ? left.timestamp : right.timestamp));
            kgsl_syncobj_destroy(&out);
            CHECK(descriptors.size() == 2);
            ++cases;
        }
    }
    kgsl_syncobj before_wrap = timestamp(&a, UINT32_MAX - 3), after_wrap = timestamp(&a, 2), out{};
    const kgsl_syncobj *wrap[] = {&before_wrap, &after_wrap};
    CHECK(merge(wrap, 2, &out) == VK_SUCCESS && out.timestamp == 2);
    kgsl_syncobj_destroy(&out);
    CHECK(merge(nullptr, 0, &out) == VK_SUCCESS && out.state == KGSL_SYNCOBJ_STATE_SIGNALED);
    kgsl_syncobj_destroy(&out);
    kgsl_syncobj pending{};
    kgsl_syncobj_init(&pending, false);
    const kgsl_syncobj *unsubmitted[] = {&pending};
    CHECK(merge(unsubmitted, 1, &out) != VK_SUCCESS);
    CHECK(out.state == KGSL_SYNCOBJ_STATE_UNSIGNALED && out.fd == -1);

    for (const auto &left : inputs) {
        for (const auto &right : inputs) {
            for (int fault = 1; fault <= 6; ++fault) {
                const kgsl_syncobj *waits[] = {&left, &right};
                operation = 0;
                fail_at = fault;
                VkResult result = merge(waits, 2, &out);
                fail_at = 0;
                if (result == VK_SUCCESS)
                    CHECK(dependencies(out) == (dependencies(left) | dependencies(right)));
                else
                    CHECK(out.state == KGSL_SYNCOBJ_STATE_UNSIGNALED && out.fd == -1);
                kgsl_syncobj_destroy(&out);
                CHECK(descriptors.size() == 2);
                ++cases;
            }
        }
    }
    for (kgsl_syncobj input : {timestamp(&a, 3), fd_a}) {
        int fd = -2;
        operation = 0;
        fail_at = 1;
        CHECK(kgsl_syncobj_export(&input, &fd) != VK_SUCCESS && fd == -1);
        fail_at = 0;
        CHECK(descriptors.size() == 2);
    }
    operation = 0;
    fail_at = 1;
    CHECK(duplicate(&fd_a, &out) != VK_SUCCESS);
    fail_at = 0;
    CHECK(out.state == KGSL_SYNCOBJ_STATE_UNSIGNALED && out.fd == -1);
    kgsl_syncobj_destroy(&out);
    CHECK(descriptors.size() == 2);
    kgsl_syncobj_destroy(&fd_a);
    kgsl_syncobj_destroy(&fd_b);
    CHECK(descriptors.empty());
    std::printf("PASS: %u merge/fault cases, timestamp wrap, empty waits, failed exports and duplicate cleanup. Software fence model; no GPU ioctls.\n", cases);
}
