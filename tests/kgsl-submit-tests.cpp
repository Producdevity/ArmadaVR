#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unistd.h>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); \
} } while (0)

enum VkResult { VK_SUCCESS = 0, VK_ERROR_OUT_OF_HOST_MEMORY = -1,
                VK_ERROR_DEVICE_LOST = -4, VK_ERROR_UNKNOWN = -13 };
enum vk_sync_flags { unused = 0 };
struct vk_sync { bool drm; };
struct vk_drm_syncobj { vk_sync base; uint32_t syncobj; };
struct kgsl_syncobj { int fd; };
struct vk_kgsl_syncobj { vk_sync vk; kgsl_syncobj syncobj; };
struct vk_sync_wait { vk_sync *sync; uint64_t stage_mask, wait_value; };
struct vk_sync_signal { vk_sync *sync; uint64_t signal_value; };
struct vk_device { bool lost; };
struct tu_physical_device { int kgsl_sync_fd; };
struct tu_device { vk_device vk; tu_physical_device *physical_device; };
struct tu_queue { tu_device *device; };
struct tu_u_trace_submission_data {};
static int vk_kgsl_sync_type;
enum Failure { none, allocation, initialization, wait_export, wait_import,
               submission, completion_export, publication };
static Failure failure;
static unsigned fail_at, calls[8], expected_waits, expected_signals;
static int borrowed_fd;
static bool empty_completion;
static std::set<int> owned;
static std::set<void *> allocations, initialized;
static std::vector<uint64_t> points;
static tu_u_trace_submission_data trace;
static int submission_token;

static bool fails(Failure operation)
{
    return ++calls[operation] == fail_at && failure == operation;
}

static int exported_fd()
{
    int fd = dup(borrowed_fd);
    CHECK(fd >= 0 && owned.insert(fd).second);
    return fd;
}

static int tracked_close(int fd)
{
    CHECK(owned.erase(fd) == 1);
    return close(fd);
}

static void *tracked_calloc(size_t count, size_t bytes)
{
    if (fails(allocation)) return nullptr;
    void *p = calloc(count, bytes);
    if (p) CHECK(allocations.insert(p).second);
    return p;
}

static void tracked_free(void *p)
{
    if (p) CHECK(allocations.erase(p) == 1);
    free(p);
}

static vk_drm_syncobj *vk_sync_as_drm_syncobj(vk_sync *sync)
{
    return sync->drm ? reinterpret_cast<vk_drm_syncobj *>(sync) : nullptr;
}

static VkResult vk_sync_init(vk_device *, vk_sync *sync, int *type, vk_sync_flags, uint64_t value)
{
    CHECK(type == &vk_kgsl_sync_type && value == 0);
    if (fails(initialization)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    CHECK(initialized.insert(sync).second);
    sync->drm = false;
    reinterpret_cast<vk_kgsl_syncobj *>(sync)->syncobj.fd = -1;
    return VK_SUCCESS;
}

static void vk_sync_finish(vk_device *, vk_sync *sync)
{
    CHECK(initialized.erase(sync) == 1);
    int fd = reinterpret_cast<vk_kgsl_syncobj *>(sync)->syncobj.fd;
    if (fd >= 0) tracked_close(fd);
}

static int tu_kgsl_drm_export_wait(int drm, uint32_t handle, uint64_t point, int *fd)
{
    CHECK(drm == 42 && handle >= 100 && handle < 100 + expected_waits);
    CHECK(point == 7 + handle - 100);
    *fd = -1;
    if (fails(wait_export)) return -1;
    *fd = exported_fd();
    return 0;
}

static VkResult kgsl_syncobj_import(kgsl_syncobj *sync, int fd)
{
    CHECK(owned.count(fd) == 1);
    if (fails(wait_import)) return VK_ERROR_UNKNOWN;
    sync->fd = fd;
    return VK_SUCCESS;
}

static VkResult kgsl_queue_submit_native(tu_queue *, void *submit,
    vk_sync_wait *waits, uint32_t wait_count, vk_sync_signal *signals,
    uint32_t signal_count, tu_u_trace_submission_data *data)
{
    CHECK(submit == &submission_token && data == &trace);
    CHECK(wait_count == expected_waits && signal_count == (expected_signals ? 1u : 0u));
    for (unsigned i = 0; i < wait_count; ++i) {
        CHECK(!waits[i].sync->drm && waits[i].wait_value == 0);
        CHECK(waits[i].stage_mask == (uint64_t(1) << i));
        CHECK(owned.count(reinterpret_cast<vk_kgsl_syncobj *>(waits[i].sync)->syncobj.fd) == 1);
    }
    if (fails(submission)) return VK_ERROR_UNKNOWN;
    if (signal_count && !empty_completion)
        reinterpret_cast<vk_kgsl_syncobj *>(signals[0].sync)->syncobj.fd = exported_fd();
    return VK_SUCCESS;
}

static VkResult kgsl_syncobj_export(kgsl_syncobj *sync, int *fd)
{
    if (fails(completion_export)) return VK_ERROR_UNKNOWN;
    CHECK((sync->fd == -1) == empty_completion);
    *fd = empty_completion ? -1 : exported_fd();
    return VK_SUCCESS;
}

static int tu_kgsl_drm_import_signal(int drm, uint32_t handle, uint64_t point, int fd)
{
    CHECK(drm == 42 && handle == 200 + points.size());
    CHECK(empty_completion ? fd == -1 : owned.count(fd) == 1);
    if (fails(publication)) return -1;
    points.push_back(point);
    return 0;
}

static VkResult vk_device_set_lost(vk_device *device, const char *)
{
    device->lost = true;
    return VK_ERROR_DEVICE_LOST;
}

#define calloc tracked_calloc
#define free tracked_free
#define close tracked_close
#include "kgsl-submit-under-test.h"
#undef calloc
#undef free
#undef close

static void run(Failure fault, unsigned at, unsigned wait_count, unsigned signal_count,
                bool empty = false, bool bad_wait = false, bool bad_signal = false)
{
    failure = fault; fail_at = at; expected_waits = wait_count; expected_signals = signal_count;
    empty_completion = empty; points.clear();
    for (unsigned &count : calls) count = 0;
    tu_physical_device physical{42}; tu_device device{{false}, &physical}; tu_queue queue{&device};
    vk_drm_syncobj inputs[3] = {{{true},100},{{true},101},{{true},102}};
    vk_drm_syncobj outputs[3] = {{{true},200},{{true},201},{{true},202}};
    vk_sync_wait waits[3]{}; vk_sync_signal signals[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        waits[i] = {&inputs[i].base, uint64_t(1) << i, 7+i};
        signals[i] = {&outputs[i].base, 11+i};
    }
    if (bad_wait) inputs[1].base.drm = false;
    if (bad_signal) outputs[1].base.drm = false;
    VkResult result = kgsl_queue_submit_drm(&queue, &submission_token,
        waits, wait_count, signals, signal_count, &trace);
    bool failed = fault != none || bad_wait || bad_signal;
    CHECK((result != VK_SUCCESS) == failed);
    CHECK(device.vk.lost == (fault == completion_export || fault == publication));
    if (device.vk.lost) CHECK(result == VK_ERROR_DEVICE_LOST && calls[submission] == 1);
    if (failed && !device.vk.lost) CHECK(points.empty());
    if (fault == allocation || fault == initialization || fault == wait_export ||
        fault == wait_import || bad_wait || bad_signal) CHECK(calls[submission] == 0);
    if (!failed) {
        CHECK(calls[submission] == 1 && points.size() == signal_count);
        for (unsigned i = 0; i < signal_count; ++i) CHECK(points[i] == 11+i);
    }
    CHECK(owned.empty() && allocations.empty() && initialized.empty());
}

int main()
{
    alarm(15);
    int pipe_fds[2]; CHECK(pipe(pipe_fds) == 0); borrowed_fd = pipe_fds[0];
    unsigned cases = 0;
    for (unsigned waits = 0; waits <= 3; ++waits)
        for (unsigned signals = 0; signals <= 3; ++signals) {
            run(none, 0, waits, signals); ++cases;
            if (!waits) { run(none, 0, waits, signals, true); ++cases; }
        }
    for (Failure fault : {allocation, initialization, wait_export, wait_import,
                          submission, completion_export, publication}) {
        unsigned count = fault == allocation ? 2 : fault == initialization ? 4 :
                         fault == submission || fault == completion_export ? 1 : 3;
        for (unsigned at = 1; at <= count; ++at) { run(fault, at, 3, 3); ++cases; }
    }
    run(none, 0, 3, 3, false, true, false); ++cases;
    run(none, 0, 3, 3, false, false, true); ++cases;
    CHECK(write(pipe_fds[1], "x", 1) == 1);
    char value; CHECK(read(borrowed_fd, &value, 1) == 1 && value == 'x');
    close(pipe_fds[0]); close(pipe_fds[1]);
    std::printf("PASS: %u actual bridge-wrapper cases, wait/signal mapping, error ordering, device loss, allocations and real FD ownership; native submission and DRM calls are modeled, no GPU execution.\n", cases);
}
