#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "quest-imu-stream.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STREAM_FILTER_IOCTL 0x40240a01UL
#define READ_BYTES 4096
#define BATCH_SAMPLES 128

struct stream_filter {
    uint64_t allowlist[4];
    uint32_t flags;
} __attribute__((packed));
_Static_assert(sizeof(struct stream_filter) == 36, "Syncboss ARM64 filter ABI");

struct quest_imu_stream {
    struct quest_imu_stream_config config;
    struct quest_imu imu;
    int fd, error;
    int64_t began, clock, last_capture;
};

static bool timestamp(int64_t *value)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 ||
        t.tv_nsec >= 1000000000 || (uint64_t)t.tv_sec > INT64_MAX / UINT64_C(1000000000))
        return false;
    int64_t seconds = (int64_t)t.tv_sec * INT64_C(1000000000);
    if (seconds > INT64_MAX - t.tv_nsec) return false;
    *value = seconds + t.tv_nsec;
    return *value > 0;
}

static int fail(struct quest_imu_stream *s, int error)
{
    if (!s->error) {
        s->error = error;
        s->config.invalidated(s->config.userdata, s->config.generation, error);
        int fd = s->fd;
        s->fd = -1;
        /* Linux releases the descriptor even when close reports an I/O error. */
        if (fd >= 0) (void)close(fd);
    }
    return s->error;
}

static bool fresh(int64_t sample, int64_t now, int64_t age)
{
    return sample > 0 && sample <= now && now - sample <= age;
}

static bool clock_now(struct quest_imu_stream *s, int64_t *now)
{
    if (!timestamp(now) || *now < s->clock) return false;
    s->clock = *now;
    return true;
}

bool quest_imu_stream_open(const struct quest_imu_stream_config *c, struct quest_imu_stream **out)
{
    if (!c || !out || *out || !c->generation || !c->sample || !c->invalidated ||
        c->max_gap_ns <= 0 || c->max_gap_ns > c->imu.max_age_ns ||
        c->imu.max_age_ns > INT64_C(1000000000)) return false;
    struct quest_imu imu = {0};
    if (!quest_imu_init(&imu, &c->imu)) return false;
    struct quest_imu_stream *s = calloc(1, sizeof(*s));
    if (!s) return false;
    s->fd = open("/dev/syncboss_stream0", O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (s->fd < 0) { free(s); return false; }
    struct stat st;
    struct stream_filter filter = {.allowlist = {[1] = UINT64_C(1) << 16}, .flags = 1};
    if (fstat(s->fd, &st) || !S_ISCHR(st.st_mode) ||
        ioctl(s->fd, STREAM_FILTER_IOCTL, &filter) || !timestamp(&s->began)) {
        (void)close(s->fd);
        free(s);
        return false;
    }
    s->clock = s->began;
    s->imu = imu;
    s->config = *c;
    *out = s;
    return true;
}

static int batch(struct quest_imu_stream *s, const unsigned char *data, size_t size, int64_t received)
{
    struct quest_imu next = s->imu;
    struct quest_imu_sample samples[BATCH_SAMPLES];
    unsigned count = 0;
    size_t pos = 0;
    int64_t last = s->last_capture;
    while (pos < size) {
        size_t left = size - pos;
        const unsigned char *p = data + pos;
        if (left < 2 || !((p[0] == 2 && p[1] == 12) || (p[0] == 3 && p[1] == 21)))
            return fail(s, -EPROTO);
        size_t header = p[1];
        if (left < header + 3 || p[2] || p[header] != 80 || p[header + 1] ||
            p[header + 2] > 252) return fail(s, -EPROTO);
        size_t length = header + 3 + p[header + 2];
        if (length > left || count == BATCH_SAMPLES) return fail(s, -EPROTO);
        struct quest_imu_sample value;
        enum quest_imu_status status = quest_imu_process_fifo(&next, p, length, received, received, &value);
        if (status != QUEST_IMU_ACCEPTED && status != QUEST_IMU_NEEDS_HISTORY)
            return fail(s, status == QUEST_IMU_STALE ? -ETIMEDOUT : -EPROTO);
        if (last && next.stream.timestamp_ns - last > s->config.max_gap_ns)
            return fail(s, -ETIMEDOUT);
        last = next.stream.timestamp_ns;
        if (status == QUEST_IMU_ACCEPTED) samples[count++] = value;
        pos += length;
    }
    s->imu = next;
    s->last_capture = last;
    for (unsigned i = 0; i < count; ++i) {
        int64_t now;
        if (!clock_now(s, &now)) return fail(s, -ERANGE);
        if (!fresh(samples[i].timestamp_ns, now, s->config.imu.max_age_ns) ||
            !fresh(samples[i].capture_timestamp_ns, now, s->config.imu.max_age_ns))
            return fail(s, -ETIMEDOUT);
        if (!s->config.sample(s->config.userdata, s->config.generation, &samples[i]))
            return fail(s, -EPIPE);
    }
    return (int)count;
}

int quest_imu_stream_poll(struct quest_imu_stream *s)
{
    if (!s) return -EINVAL;
    if (s->error) return s->error;
    int delivered = 0;
    for (unsigned read_count = 0; read_count < 8; ++read_count) {
        int64_t now;
        if (!clock_now(s, &now)) return fail(s, -ERANGE);
        if (!fresh(s->last_capture ? s->last_capture : s->began, now, s->config.imu.max_age_ns))
            return fail(s, -ETIMEDOUT);
        unsigned char data[READ_BYTES];
        ssize_t size = read(s->fd, data, sizeof(data));
        int error = errno;
        if (!clock_now(s, &now)) return fail(s, -ERANGE);
        if (size < 0) {
            if (error == EINTR) continue;
            if (error == EAGAIN || error == EWOULDBLOCK) break;
            return fail(s, error > 0 ? -error : -EIO);
        }
        if (!size || (size_t)size > sizeof(data)) return fail(s, -EIO);
        int count = batch(s, data, (size_t)size, now);
        if (count < 0) return count;
        delivered += count;
    }
    int64_t now;
    if (!clock_now(s, &now)) return fail(s, -ERANGE);
    if (!fresh(s->last_capture ? s->last_capture : s->began, now, s->config.imu.max_age_ns))
        return fail(s, -ETIMEDOUT);
    return delivered;
}

void quest_imu_stream_stop(struct quest_imu_stream *s)
{
    if (s) (void)fail(s, -ECANCELED);
}

void quest_imu_stream_destroy(struct quest_imu_stream **s)
{
    if (!s || !*s) return;
    quest_imu_stream_stop(*s);
    free(*s);
    *s = NULL;
}
