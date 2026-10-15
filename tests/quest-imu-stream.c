#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "quest-imu-stream.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static struct {
    unsigned char bytes[4096];
    size_t size;
    unsigned opens, closes, reads, filters, samples, invalidations, clocks;
    int open_error, stat_error, filter_error, read_error, clock_error, invalid_error;
    bool regular, reject_sample, close_error;
    int64_t now, read_delay, sample_delay;
    struct quest_imu_sample received[128];
} mock;
static unsigned cases;
static const struct imu_temperature_offset offsets[] = {{25, {.1f, -.2f, .3f}}};

int quest_test_open(const char *path, int flags, ...)
{
    ++mock.opens;
    assert(!strcmp(path, "/dev/syncboss_stream0"));
    assert(flags == (O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if (mock.open_error) { errno = mock.open_error; return -1; }
    return 73;
}
int quest_test_fstat(int fd, struct stat *st)
{
    assert(fd == 73);
    if (mock.stat_error) { errno = mock.stat_error; return -1; }
    memset(st, 0, sizeof(*st)); st->st_mode = mock.regular ? S_IFREG : S_IFCHR;
    return 0;
}
int quest_test_ioctl(int fd, unsigned long command, ...)
{
    assert(fd == 73 && command == 0x40240a01UL); ++mock.filters;
    va_list args; va_start(args, command); const unsigned char *p = va_arg(args, const void *); va_end(args);
    for (unsigned i = 0; i < 36; ++i) assert(p[i] == (i == 10 || i == 32 ? 1 : 0));
    if (mock.filter_error) { errno = mock.filter_error; return -1; }
    return 0;
}
ssize_t quest_test_read(int fd, void *data, size_t size)
{
    assert(fd == 73 && size == 4096); ++mock.reads; mock.now += mock.read_delay;
    if (mock.read_error) { errno = mock.read_error; return -1; }
    if (!mock.size) { errno = EAGAIN; return -1; }
    memcpy(data, mock.bytes, mock.size); ssize_t result = (ssize_t)mock.size; mock.size = 0;
    return result;
}
int quest_test_close(int fd)
{
    assert(fd == 73); ++mock.closes;
    if (mock.close_error) { errno = EINTR; return -1; }
    return 0;
}
int quest_test_clock(clockid_t clock, struct timespec *t)
{
    assert(clock == CLOCK_MONOTONIC); ++mock.clocks;
    if (mock.clock_error) { errno = EIO; return -1; }
    t->tv_sec = mock.now / 1000000000; t->tv_nsec = mock.now % 1000000000;
    return 0;
}
static bool sample(void *userdata, uint64_t generation, const struct quest_imu_sample *value)
{
    assert(userdata == &mock && generation == 42 && !mock.invalidations && mock.samples < 128);
    mock.received[mock.samples++] = *value;
    mock.now += mock.sample_delay;
    return !mock.reject_sample;
}
static void invalidated(void *userdata, uint64_t generation, int error)
{
    assert(userdata == &mock && generation == 42 && error < 0 && !mock.invalidations);
    ++mock.invalidations; mock.invalid_error = error;
}
static struct quest_imu_stream_config configuration(void)
{
    return (struct quest_imu_stream_config){
        .imu = {.format = 2, .parameters = {16.384f, 4096, 25, 128, false},
            .max_age_ns = 5000000, .frame = IMU_FRAME_ACCELEROMETER,
            .accelerometer = {0, -3, 0, 2, 0, 0, 0, 0, 4},
            .gyroscope = {1, .25, 0, 0, 2, .125, -.1, 0, 3},
            .device_from_imu = {1, 0, 0, 0, 0, -1, 0, 1, 0},
            .accelerometer_offsets = offsets, .gyroscope_offsets = offsets,
            .accelerometer_offset_count = 1, .gyroscope_offset_count = 1},
        .generation = 42, .max_gap_ns = 2000000, .userdata = &mock,
        .sample = sample, .invalidated = invalidated,
    };
}
static void reset(void) { memset(&mock, 0, sizeof(mock)); mock.now = 1000000000; }
static void put(unsigned char *p, unsigned size, uint64_t n)
{ for (unsigned i = 0; i < size; ++i) p[i] = (unsigned char)(n >> (8*i)); }
static void put_float(unsigned char *p, float f)
{ uint32_t n; memcpy(&n, &f, sizeof(n)); put(p, 4, n); }
static size_t packet(unsigned char *p, unsigned version, unsigned format, uint64_t ticks)
{
    size_t h = version == 2 ? 12 : 21, payload = format == 2 || format == 3 ? 22 : 36;
    memset(p, 0, h + 3 + payload); p[0] = version; p[1] = h; p[3] = 1;
    put(p+4, 8, 1000000); p[h] = 80; p[h+2] = payload;
    put(p+h+3, 8, format >= 3 ? ticks << 16 : ticks);
    if (payload == 22) for (unsigned i = 0; i < 7; ++i) put(p+h+11+2*i, 2, 10+i);
    else for (unsigned i = 0; i < 7; ++i) put_float(p+h+11+4*i, (float)(10+i));
    return h + 3 + payload;
}
static void same(const struct quest_imu_sample *a, const struct quest_imu_sample *b)
{
    assert(a->timestamp_ns == b->timestamp_ns && a->capture_timestamp_ns == b->capture_timestamp_ns);
    assert(a->delivery_ns == b->delivery_ns && !a->counter && !b->counter);
    assert(a->sample.timestamp_ticks == b->sample.timestamp_ticks);
    assert(a->sample.temperature_c == b->sample.temperature_c);
    assert(!memcmp(a->sample.acceleration_m_s2, b->sample.acceleration_m_s2, 12));
    assert(!memcmp(a->sample.angular_velocity_rad_s, b->sample.angular_velocity_rad_s, 12));
}
static void terminal(struct quest_imu_stream **s, int error)
{
    assert(quest_imu_stream_poll(*s) == error);
    assert(mock.invalidations == 1 && mock.invalid_error == error && mock.closes == 1);
    unsigned reads = mock.reads;
    assert(quest_imu_stream_poll(*s) == error && mock.reads == reads);
    quest_imu_stream_stop(*s); quest_imu_stream_destroy(s);
    assert(!*s && mock.closes == 1 && mock.invalidations == 1); ++cases;
}
static void comparisons(void)
{
    for (unsigned format = 1; format <= 4; ++format) for (unsigned version = 2; version <= 3; ++version)
        for (unsigned aligned = 0; aligned < 2; ++aligned) {
            reset(); struct quest_imu_stream_config c = configuration();
            struct quest_imu_timing timing = {-625000, 0, 2000000};
            c.imu.format = format; c.imu.timing = aligned ? &timing : NULL;
            struct quest_imu ref = {0}; assert(quest_imu_init(&ref, &c.imu));
            struct quest_imu_stream *s = NULL; assert(quest_imu_stream_open(&c, &s));
            for (unsigned i = 1; i <= 64; ++i) {
                mock.size = packet(mock.bytes, version, format, i*1250);
                mock.now = 1000000000 + i*1250000 + 100000;
                struct quest_imu_sample expected;
                enum quest_imu_status result = quest_imu_process_fifo(&ref, mock.bytes, mock.size, mock.now, mock.now, &expected);
                assert(result == QUEST_IMU_ACCEPTED || result == QUEST_IMU_NEEDS_HISTORY);
                unsigned count = mock.samples;
                assert(quest_imu_stream_poll(s) == (result == QUEST_IMU_ACCEPTED));
                if (result == QUEST_IMU_ACCEPTED) same(&mock.received[count], &expected);
                ++cases;
            }
            quest_imu_stream_stop(s); terminal(&s, -ECANCELED);
        }
}
static void malformed(void)
{
    for (unsigned length = 1; length < 37; ++length) {
        reset(); struct quest_imu_stream_config c = configuration();
        struct quest_imu_stream *s = NULL; assert(quest_imu_stream_open(&c, &s));
        size_t n = packet(mock.bytes, 2, 2, 1000);
        packet(mock.bytes+n, 2, 2, 2000); mock.size = n+length; mock.now += 2100000;
        terminal(&s, -EPROTO); assert(!mock.samples);
    }
    for (unsigned change = 0; change < 8; ++change) {
        reset(); struct quest_imu_stream_config c = configuration();
        struct quest_imu_stream *s = NULL; assert(quest_imu_stream_open(&c, &s));
        mock.size = packet(mock.bytes, 2, 2, 1000); mock.now += 1100000;
        const unsigned fields[] = {0,1,2,3,12,13,14,14};
        const unsigned values[] = {7,21,1,0,81,1,253,0};
        mock.bytes[fields[change]] = values[change];
        terminal(&s, -EPROTO); assert(!mock.samples);
    }
}
static void failures(void)
{
    for (unsigned failure = 0; failure < 6; ++failure) {
        reset(); struct quest_imu_stream_config c = configuration(); struct quest_imu_stream *s = NULL;
        if (failure == 0) mock.open_error = EACCES;
        if (failure == 1) mock.stat_error = EIO;
        if (failure == 2) mock.regular = true;
        if (failure == 3) mock.filter_error = ENOTTY;
        if (failure == 4) mock.clock_error = 1;
        if (failure == 5) mock.now = 0;
        assert(!quest_imu_stream_open(&c, &s) && !s && !mock.invalidations);
        assert(mock.closes == (failure != 0)); ++cases;
    }
    for (unsigned failure = 0; failure < 10; ++failure) {
        reset(); struct quest_imu_stream_config c = configuration(); struct quest_imu_stream *s = NULL;
        assert(quest_imu_stream_open(&c, &s)); int error = -ETIMEDOUT;
        if (failure == 0) mock.now += 5000001;
        if (failure == 1) { --mock.now; error = -ERANGE; }
        if (failure == 2) { mock.clock_error = 1; error = -ERANGE; }
        if (failure == 3) { mock.read_error = ENODEV; error = -ENODEV; }
        if (failure == 4) { mock.read_error = ECANCELED; error = -ECANCELED; }
        if (failure == 5) { mock.read_error = EINTR; mock.read_delay = 1000000; }
        if (failure == 6) mock.read_delay = 5000001;
        if (failure >= 7) {
            size_t n = packet(mock.bytes, 2, 2, 1000); packet(mock.bytes+n, 2, 2, 2000);
            mock.size = 2*n; mock.now += 2100000;
            if (failure == 7) { mock.reject_sample = true; error = -EPIPE; }
            if (failure == 8) mock.sample_delay = 5000001;
            if (failure == 9) { mock.close_error = true; mock.reject_sample = true; error = -EPIPE; }
        }
        terminal(&s, error);
    }
    reset(); struct quest_imu_stream_config c = configuration(); struct quest_imu_stream *s = NULL;
    assert(quest_imu_stream_open(&c, &s)); mock.read_error = EINTR;
    assert(quest_imu_stream_poll(s) == 0 && mock.reads == 8);
    mock.read_error = 0; mock.size = packet(mock.bytes, 2, 2, 1000); mock.now += 1100000;
    assert(quest_imu_stream_poll(s) == 1); assert(mock.samples == 1);
    mock.size = packet(mock.bytes, 2, 2, 4000); mock.now = 1004100000;
    terminal(&s, -ETIMEDOUT); assert(mock.samples == 1);
}
int main(void)
{
    comparisons(); malformed(); failures();
    printf("QUEST_IMU_STREAM cases=%u passed\n", cases);
    return 0;
}
