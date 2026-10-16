#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "quest-camera-stream.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct stream_filter { uint64_t allowlist[4]; uint32_t flags; } __attribute__((packed));
_Static_assert(sizeof(struct stream_filter) == 36, "Syncboss ARM64 filter ABI");
struct quest_camera_stream {
    struct quest_camera_stream_config config;
    int fd, error;
    int64_t clock, received[4];
    uint64_t timestamp[4];
    int64_t timestamp_ns[4], sof_ns[4];
};
static bool now_ns(int64_t *value)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 ||
        t.tv_nsec >= 1000000000 || t.tv_sec > (INT64_MAX - t.tv_nsec) / INT64_C(1000000000)) return false;
    *value = t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
    return *value > 0;
}
static int fail(struct quest_camera_stream *s, int error)
{
    if (!s->error) {
        s->error = error;
        s->config.invalidated(s->config.userdata, s->config.generation, error);
        int fd = s->fd; s->fd = -1;
        if (fd >= 0) (void)close(fd);
    }
    return s->error;
}
static int check_clock(struct quest_camera_stream *s, int64_t *now)
{
    if (!now_ns(now) || *now < s->clock) return fail(s, -ERANGE);
    s->clock = *now;
    for (size_t i = 0; i < s->config.camera_count; ++i)
        if (*now - s->received[i] >= s->config.silence_ns) return fail(s, -ETIMEDOUT);
    return 0;
}
bool quest_camera_stream_open(const struct quest_camera_stream_config *c,
                              struct quest_camera_stream **stream)
{
    if (!c || !stream || *stream || !c->generation || !c->camera_count || c->camera_count > 4 ||
        c->silence_ns <= 0 || c->silence_ns > INT64_C(1000000000) ||
        c->max_age_ns <= 0 || c->max_age_ns > INT64_C(1000000000) ||
        !c->max_gap_us || c->max_gap_us > UINT64_C(1000000) || !c->shutter || !c->invalidated) return false;
    uint32_t mask = 0;
    for (size_t i = 0; i < c->camera_count; ++i) {
        uint32_t bit = c->camera_bits[i];
        if (!bit || (bit & (bit - 1u)) || (mask & bit)) return false;
        mask |= bit;
    }
    struct quest_camera_stream *s = calloc(1, sizeof(*s));
    if (!s) return false;
    s->fd = open("/dev/syncboss_stream0", O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (s->fd < 0) { free(s); return false; }
    struct stat st;
    struct stream_filter filter = {.allowlist = {[3] = UINT64_C(1) << 62}, .flags = 1};
    if (fstat(s->fd, &st) || !S_ISCHR(st.st_mode) ||
        ioctl(s->fd, 0x40240a01UL, &filter) || !now_ns(&s->clock)) {
        (void)close(s->fd); free(s); return false;
    }
    s->config = *c;
    for (size_t i = 0; i < c->camera_count; ++i) s->received[i] = s->clock;
    *stream = s;
    return true;
}
static int batch(struct quest_camera_stream *s, const uint8_t *data, size_t size, int64_t received)
{
    struct { struct quest_camera_shutter_record record; unsigned index; } values[160];
    uint64_t timestamps[4]; memcpy(timestamps, s->timestamp, sizeof(timestamps));
    int64_t times[4], sofs[4];
    memcpy(times, s->timestamp_ns, sizeof(times)); memcpy(sofs, s->sof_ns, sizeof(sofs));
    size_t pos = 0, count = 0;
    while (pos < size) {
        const uint8_t *p = data + pos;
        size_t left = size - pos;
        if (left < 3 || !((p[0] == 2 && p[1] == 12) || (p[0] == 3 && p[1] == 21))) return fail(s, -EPROTO);
        size_t header = p[1];
        if (left < header || p[2] > 1) return fail(s, -EPROTO);
        if (p[2]) return fail(s, -EPIPE);
        if (left < header + 3 || p[header] != 254 || p[header + 1] || p[header + 2] > 252) return fail(s, -EPROTO);
        size_t payload_size = p[header + 2], length = header + 3 + payload_size;
        if (length > left || payload_size < 5) return fail(s, -EPROTO);
        const uint8_t *payload = p + header + 3;
        uint32_t mask = 0;
        for (unsigned i = 0; i < 4; ++i) mask |= (uint32_t)payload[1 + i] << (8 * i);
        struct quest_camera_shutter_record checked;
        if (!mask || !quest_camera_shutter_decode_fifo(p, length, mask & (0u - mask), &checked)) return fail(s, -EPROTO);
        for (unsigned i = 0; i < s->config.camera_count; ++i) {
            if (!(mask & s->config.camera_bits[i])) continue;
            if (count == 160 || !quest_camera_shutter_decode_fifo(p, length, s->config.camera_bits[i], &values[count].record))
                return fail(s, -EPROTO);
            const struct quest_camera_shutter_record *record = &values[count].record;
            uint64_t t = record->shutter.timestamp_us;
            if (record->timestamp_ns <= times[i] || record->sof_ns <= sofs[i]) return fail(s, -ERANGE);
            if (record->timestamp_ns > received || record->sof_ns > received ||
                received - record->timestamp_ns > s->config.max_age_ns ||
                received - record->sof_ns > s->config.max_age_ns) return fail(s, -ETIMEDOUT);
            times[i] = record->timestamp_ns; sofs[i] = record->sof_ns;
            if (timestamps[i] && (t <= timestamps[i] || t - timestamps[i] > s->config.max_gap_us)) return fail(s, -ERANGE);
            timestamps[i] = t; values[count++].index = i;
        }
        pos += length;
    }
    memcpy(s->timestamp, timestamps, sizeof(timestamps));
    memcpy(s->timestamp_ns, times, sizeof(times)); memcpy(s->sof_ns, sofs, sizeof(sofs));
    for (size_t i = 0; i < count; ++i) s->received[values[i].index] = received;
    for (size_t i = 0; i < count; ++i) {
        int64_t now;
        if (check_clock(s, &now)) return s->error;
        if (now - received >= s->config.silence_ns ||
            now - values[i].record.timestamp_ns > s->config.max_age_ns ||
            now - values[i].record.sof_ns > s->config.max_age_ns) return fail(s, -ETIMEDOUT);
        if (!s->config.shutter(s->config.userdata, s->config.generation,
            s->config.camera_bits[values[i].index], &values[i].record, received)) return fail(s, -EPIPE);
    }
    return (int)count;
}
int quest_camera_stream_poll(struct quest_camera_stream *s)
{
    if (!s) return -EINVAL;
    if (s->error) return s->error;
    int delivered = 0;
    for (unsigned i = 0; i < 8; ++i) {
        int64_t now;
        if (check_clock(s, &now)) return s->error;
        uint8_t data[4096];
        ssize_t size = read(s->fd, data, sizeof(data)); int error = errno;
        if (check_clock(s, &now)) return s->error;
        if (size < 0) {
            if (error == EINTR) continue;
            if (error == EAGAIN || error == EWOULDBLOCK) break;
            return fail(s, error > 0 ? -error : -EIO);
        }
        if (!size || (size_t)size > sizeof(data)) return fail(s, -EPIPE);
        int n = batch(s, data, (size_t)size, now);
        if (n < 0) return n;
        delivered += n;
    }
    int64_t now;
    return check_clock(s, &now) ? s->error : delivered;
}
void quest_camera_stream_stop(struct quest_camera_stream *s)
{ if (s) (void)fail(s, -ECANCELED); }
void quest_camera_stream_destroy(struct quest_camera_stream **stream)
{
    if (stream && *stream) { quest_camera_stream_stop(*stream); free(*stream); *stream = NULL; }
}
