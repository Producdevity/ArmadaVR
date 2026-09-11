#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <set>
#include <thread>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::exit(1); \
} } while (0)

enum VkResult { VK_SUCCESS = 0, VK_TIMEOUT = 2, VK_ERROR_OUT_OF_HOST_MEMORY = -1, VK_ERROR_UNKNOWN = -13 };
struct vk_object_base { uint64_t unused; };
struct tu_device { int fd; };
struct tu_queue { unsigned msm_queue_id; int read_fd; };
struct kgsl_syncobj;
static std::set<int> owned;
static int export_count, export_failure, interrupts, poll_failure;
static bool allocation_failure;
static uint32_t waited_timestamp;
static unsigned waited_queue, single_waits;

static int timestamp_to_fd(tu_queue *queue, uint32_t)
{
    CHECK(queue != nullptr);
    if (++export_count == export_failure) {
        errno = EIO;
        return -1;
    }
    int fd = dup(queue->read_fd);
    CHECK(fd >= 0 && owned.insert(fd).second);
    return fd;
}

static int close_export(int fd)
{
    CHECK(owned.erase(fd) == 1);
    return close(fd);
}

static int interruptible_poll(pollfd *fds, nfds_t count, int timeout)
{
    if (interrupts) {
        errno = (--interrupts % 2) ? EINTR : EAGAIN;
        return -1;
    }
    if (poll_failure) {
        errno = poll_failure;
        return -1;
    }
    return poll(fds, count, timeout);
}

static void *checked_calloc(size_t count, size_t bytes)
{
    return allocation_failure ? nullptr : calloc(count, bytes);
}

static uint64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int get_relative_ms(uint64_t deadline)
{
    if (deadline == UINT64_MAX) return -1;
    uint64_t now = now_ns();
    return deadline > now ? static_cast<int>((deadline - now + 999999) / 1000000) : 0;
}

static VkResult wait_timestamp_safe(int, unsigned queue, unsigned timestamp, uint64_t)
{
    waited_queue = queue;
    waited_timestamp = timestamp;
    return VK_TIMEOUT;
}

static VkResult kgsl_syncobj_wait(tu_device *, kgsl_syncobj *, uint64_t)
{
    ++single_waits;
    return VK_TIMEOUT;
}

#if OLD_KGSL_WAIT
struct u_vector { void *data; uint32_t count; size_t element; };
static void u_vector_init(u_vector *v, unsigned, size_t element)
{
    v->element = element;
}
static void *u_vector_add(u_vector *v)
{
    v->data = realloc(v->data, ++v->count * v->element);
    CHECK(v->data != nullptr);
    return static_cast<char *>(v->data) + (v->count - 1) * v->element;
}
static unsigned u_vector_length(u_vector *v) { return v->count; }
static void u_vector_finish(u_vector *v) { free(v->data); }
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#endif

#define close close_export
#define poll interruptible_poll
#define calloc checked_calloc
#include "kgsl-wait-under-test.h"
#undef close
#undef poll
#undef calloc

int main()
{
    int a[2], b[2];
    CHECK(pipe(a) == 0 && pipe(b) == 0);
    tu_device device{-1};
    tu_queue first{1, a[0]}, second{2, b[0]};
    kgsl_syncobj fd_a{}, fd_b{}, ts_a{}, ts_b{}, pending{}, signaled{};
    fd_a.state = fd_b.state = KGSL_SYNCOBJ_STATE_FD;
    fd_a.fd = a[0]; fd_b.fd = b[0];
    ts_a.state = ts_b.state = KGSL_SYNCOBJ_STATE_TS;
    ts_a.queue = &first; ts_a.timestamp = 9;
    ts_b.queue = &second; ts_b.timestamp = 3;
    pending.state = KGSL_SYNCOBJ_STATE_UNSIGNALED;
    signaled.state = KGSL_SYNCOBJ_STATE_SIGNALED;
    kgsl_syncobj *fds[] = {&fd_a, &fd_b};
    auto wait = [&](kgsl_syncobj **items, unsigned count, uint64_t deadline = 0) {
        VkResult result = kgsl_syncobj_wait_any(&device, items, count, deadline);
        CHECK(owned.empty());
        return result;
    };
    CHECK(wait(nullptr, 0) == VK_TIMEOUT);
    CHECK(wait(fds, 1) == VK_TIMEOUT && single_waits == 1);
    CHECK(wait(fds, 2) == VK_TIMEOUT);
    CHECK(write(b[1], "s", 1) == 1);
    CHECK(wait(fds, 2) == VK_SUCCESS);
    interrupts = 2;
    CHECK(wait(fds, 2, now_ns() + 100000000) == VK_SUCCESS && interrupts == 0);
    char byte;
    CHECK(read(b[0], &byte, 1) == 1);
    std::thread signal([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(write(a[1], "s", 1) == 1);
    });
    CHECK(wait(fds, 2, now_ns() + 1000000000) == VK_SUCCESS);
    signal.join();
    CHECK(read(a[0], &byte, 1) == 1);

    kgsl_syncobj *mixed[] = {&ts_a, &fd_b};
    CHECK(wait(mixed, 2) == VK_TIMEOUT);
    CHECK(write(a[1], "s", 1) == 1);
    CHECK(wait(mixed, 2) == VK_SUCCESS);
    kgsl_syncobj *queues[] = {&ts_a, &ts_b};
    CHECK(wait(queues, 2) == VK_SUCCESS);
    CHECK(read(a[0], &byte, 1) == 1);
    CHECK(wait(queues, 2) == VK_TIMEOUT);
    for (int fail = 1; fail <= 2; ++fail) {
        export_count = 0; export_failure = fail;
        CHECK(wait(queues, 2) == VK_ERROR_UNKNOWN);
    }
    export_failure = 0;
    allocation_failure = true;
    CHECK(wait(fds, 2) == VK_ERROR_OUT_OF_HOST_MEMORY);
    allocation_failure = false;
    poll_failure = EIO;
    CHECK(wait(mixed, 2) == VK_ERROR_UNKNOWN);
    poll_failure = 0;
    int original = fd_b.fd;
    fd_b.fd = 2147483000;
    CHECK(wait(fds, 2) == VK_ERROR_UNKNOWN);
    fd_b.fd = original;

    ts_b.queue = &first;
    CHECK(wait(queues, 2) == VK_TIMEOUT && waited_timestamp == 3 && waited_queue == 1);
    ts_a.timestamp = UINT32_MAX - 3; ts_b.timestamp = 2;
    CHECK(wait(queues, 2) == VK_TIMEOUT && waited_timestamp == UINT32_MAX - 3);
    kgsl_syncobj *unsubmitted[] = {&pending, &pending};
    CHECK(wait(unsubmitted, 2) == VK_TIMEOUT);
    kgsl_syncobj *done[] = {&pending, &signaled};
    CHECK(wait(done, 2) == VK_SUCCESS);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
    std::puts("PASS: real poll readiness, timeout, delayed signal, interrupted waits, FD-only/mixed/multiple queues, timestamp order/wrap, empty/pending waits, allocation/export/poll failures and descriptor ownership. Pipes model fences; no GPU ioctls.");
}
