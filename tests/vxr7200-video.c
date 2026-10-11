#include "vxr7200-video.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct capture {
    uint32_t values[5];
    unsigned calls, fail_at, changed;
    int failure;
};

static int transfer(void *context, const uint8_t *tx, size_t tx_size,
                    uint8_t *rx, size_t rx_size)
{
    static const uint32_t registers[] = {0x220830, 0x220838, 0x220814, 0x220804, 0x200008};
    struct capture *capture = context;
    unsigned call = capture->calls++;
    assert(call < 10 && tx_size == 4 && rx_size == 5 && rx);
    for (unsigned byte = 0; byte < 4; ++byte)
        assert(tx[byte] == (uint8_t)(registers[call % 5] >> (byte * 8)));
    uint32_t value = capture->values[call % 5];
    if (call >= 5 && capture->changed == call - 4)
        value ^= 1;
    for (unsigned byte = 0; byte < 4; ++byte)
        rx[byte] = (uint8_t)(value >> (byte * 8));
    rx[4] = 0xff;
    return capture->fail_at == call + 1 ? capture->failure : 2;
}

int main(void)
{
    struct capture capture = {0};
    struct vxr7200_bus bus = {&capture, transfer}, missing = {0};
    struct vxr7200_video_status result, before;
    memset(&result, 0xa5, sizeof(result)); memcpy(&before, &result, sizeof(before));
    assert(vxr7200_read_video_status(NULL, &result) == -EINVAL);
    assert(vxr7200_read_video_status(&missing, &result) == -EINVAL);
    assert(vxr7200_read_video_status(&bus, NULL) == -EINVAL);
    assert(!memcmp(&before, &result, sizeof(result)) && !capture.calls);
    int failures[] = {-EIO, -ENXIO, -ETIMEDOUT, -EAGAIN, -EINTR, 0, 1, 3};
    for (unsigned call = 1; call <= 10; ++call)
        for (unsigned error = 0; error < sizeof(failures) / sizeof(*failures); ++error) {
            capture.calls = 0; capture.fail_at = call; capture.failure = failures[error];
            int expected = capture.failure < 0 ? capture.failure : -EIO;
            assert(vxr7200_read_video_status(&bus, &result) == expected);
            assert(capture.calls == call && !memcmp(&before, &result, sizeof(result)));
        }
    capture.fail_at = 0;
    for (unsigned changed = 1; changed <= 5; ++changed) {
        capture.calls = 0; capture.changed = changed;
        assert(vxr7200_read_video_status(&bus, &result) == -EAGAIN);
        assert(capture.calls == 10 && !memcmp(&before, &result, sizeof(result)));
    }
    capture.changed = 0;
    uint32_t random = 0x7200abcd;
    unsigned cases = 0;
    uint32_t modes[] = {0x0f000870, 0x0e500780, 0x0e500f00, 0x08700f00, 0, 0xffffffff};
    for (unsigned i = 0; i < 4096; ++i) {
        for (unsigned reg = 0; reg < 5; ++reg) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            capture.values[reg] = random;
        }
        capture.values[1] = modes[i % 6];
        capture.calls = 0;
        assert(!vxr7200_read_video_status(&bus, &result) && capture.calls == 10);
        assert(result.stock_dimensions == (i % 6 < 3));
        assert(result.horizontal_active == (capture.values[1] & 0xffff));
        assert(result.vertical_active == capture.values[1] >> 16);
        assert(result.horizontal_total == (capture.values[0] & 0xffff));
        assert(result.vertical_total == capture.values[0] >> 16);
        assert(result.frame_ticks == capture.values[2]);
        assert(result.clock_multiplier == ((capture.values[4] >> 16) & 1023));
        assert(result.clock_divider == (capture.values[3] & 255));
        assert(result.rate_numerator == UINT64_C(1000000) * result.clock_multiplier * result.clock_divider);
        assert(result.rate_valid == (result.frame_ticks && result.rate_numerator));
        ++cases;
    }
    capture.values[2] = capture.values[3] = capture.values[4] = UINT32_MAX;
    capture.calls = 0;
    assert(!vxr7200_read_video_status(&bus, &result));
    assert(result.rate_numerator == UINT64_C(260865000000) && result.rate_valid);
    for (unsigned reg = 2; reg < 5; ++reg) {
        capture.values[reg] = 0; capture.calls = 0;
        assert(!vxr7200_read_video_status(&bus, &result) && !result.rate_valid);
        capture.values[reg] = UINT32_MAX;
    }
    printf("%u VXR7200 video observations and failure checks passed\n", cases);
    return 0;
}
