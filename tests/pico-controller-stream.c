#include "pico-controller-stream.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *records[] = {
    "a501b289a1d21aff141f7f48b97493dc47f5268adef438d950e56d3094656265708bb3258e38d5e1556e8293084f56b5a457"
    "dc5eebc18f57c51f0ea02493c385e4bc1a6fb305af123aa046e94e56c75a3038bfd34adf8649e119e83de9ba2ab8165fed9e",
    "a5011fb068b2e3bff317f3f217dd1beb514b0fa2f6d41558960c3021ab435effd07d87a8d36ae1f4a0a339a913880e260239"
    "dff4c8a96dcaa94b5653a49b475e5d1c55fb80c669e503b856c1006759a4867df1cfc1a13b3f00fe8c03e29e7b9f8e9a5c1a",
};
static unsigned char payload[2][104];
static const uint64_t anchor = UINT64_C(0x123456789abcdef);
static unsigned rejected;

static bool receive(struct pico_controller_stream *s, uint64_t generation, uint32_t id,
                    uint64_t high, uint64_t low, uint64_t ticks, unsigned hand, size_t length,
                    int64_t now, struct pico_controller_event *event)
{
    unsigned char before[sizeof(*s)], output[sizeof(*event)];
    memcpy(before, s, sizeof(*s));
    memcpy(output, event, sizeof(*event));
    bool accepted = pico_controller_stream_report(s, generation, id, high, low, ticks,
                                                  payload[hand], length, now, event);
    if (!accepted) {
        assert(!memcmp(before, s, sizeof(*s)));
        assert(!memcmp(output, event, sizeof(*event)));
        rejected++;
    }
    return accepted;
}

int main(void)
{
    for (unsigned hand = 0; hand < 2; hand++) {
        payload[hand][0] = 0x0a; payload[hand][1] = 100;
        payload[hand][102] = 0x10; payload[hand][103] = 3;
        for (unsigned i = 0; i < 100; i++) {
            unsigned byte;
            assert(sscanf(records[hand] + 2 * i, "%2x", &byte) == 1);
            payload[hand][i + 2] = byte;
        }
    }
    struct pico_controller_stream stream = {0};
    struct pico_controller_stream_config config = {
        .suid_high = 1, .suid_low = 2, .mcu_type = 1, .radio_type = 1, .max_age_ns = 1000000,
    };
    assert(ssc_clock_init(&config.clock, anchor, 19200000, 1000000000, 1000000000, 0));
    struct pico_controller_event event;
    memset(&event, 0x5a, sizeof(event));
    assert(!receive(&stream, 0, 1024, 1, 2, anchor, 0, 104, 1000000100, &event));
    assert(pico_controller_stream_begin(&stream, &config));
    uint64_t generation = stream.generation;
    assert(generation == 1);
    assert(receive(&stream, generation, 1024, 1, 2, anchor + 3, 0, 104, 1000000200, &event));
    assert(event.sample.hand == 1 && event.sample.trigger == 0x89 && event.accuracy == 3);
    assert(event.time.uncertainty_ns == 2 && event.generation == generation);
    assert(receive(&stream, generation, 1024, 1, 2, anchor + 1, 1, 104, 1000000200, &event));
    assert(event.sample.hand == 2 && event.sample.trigger == 0xb0);
    assert(!receive(&stream, generation, 1024, 1, 2, anchor + 3, 0, 104, 1000000300, &event));
    assert(!receive(&stream, generation, 1024, 1, 2, anchor + 2, 0, 104, 1000000300, &event));
    assert(!receive(&stream, generation, 1024, 1, 2, anchor + 4, 0, 103, 1000000300, &event));
    assert(receive(&stream, generation, 1024, 1, 2, anchor + 4, 0, 104, 1000000300, &event));
    assert(!receive(&stream, generation, 1024, 1, 2, anchor + 5, 0, 104, 1000000299, &event));
    for (unsigned id = 0; id < 2048; id++) {
        if (id != 1024)
            assert(!receive(&stream, generation, id, 1, 2, anchor + 5, 0, 104, 1000000400, &event));
    }
    assert(!receive(&stream, generation, 1024, 2, 2, anchor + 5, 0, 104, 1000000400, &event));
    assert(!receive(&stream, generation, 1024, 1, 3, anchor + 5, 0, 104, 1000000400, &event));
    assert(!receive(&stream, generation, 1024, 1, 2, anchor + 20000, 0, 104, 1000000400, &event));
    assert(!receive(&stream, generation, 1024, 1, 2, anchor + 5, 0, 104, 2000000000, &event));
    pico_controller_stream_invalidate(&stream);
    assert(!receive(&stream, generation, 1024, 1, 2, anchor + 5, 0, 104, 1000000400, &event));
    assert(pico_controller_stream_begin(&stream, &config));
    assert(stream.generation == generation + 1);
    assert(!receive(&stream, generation, 1024, 1, 2, anchor + 5, 0, 104, 1000000400, &event));
    assert(receive(&stream, stream.generation, 1024, 1, 2, anchor, 0, 104, 1000000400, &event));
    for (unsigned cycle = 0; cycle < 1000; cycle++) {
        uint64_t old = stream.generation;
        pico_controller_stream_invalidate(&stream);
        assert(pico_controller_stream_begin(&stream, &config));
        assert(!receive(&stream, old, 1024, 1, 2, anchor + 100, 0, 104, 1000010000, &event));
        assert(receive(&stream, stream.generation, 1024, 1, 2, anchor, cycle % 2, 104, 1000010000, &event));
    }
    struct pico_controller_stream saved = stream;
    for (unsigned type = 0; type < 5; type++) {
        if (type == 1 || type == 2) continue;
        config.mcu_type = type;
        assert(!pico_controller_stream_begin(&stream, &config));
        assert(!memcmp(&stream, &saved, sizeof(stream)));
    }
    config.mcu_type = 1; config.clock.initialized = false;
    assert(!pico_controller_stream_begin(&stream, &config));
    assert(!memcmp(&stream, &saved, sizeof(stream)));
    config.clock.initialized = true;
    stream.generation = UINT64_MAX;
    saved = stream;
    assert(!pico_controller_stream_begin(&stream, &config));
    assert(!memcmp(&stream, &saved, sizeof(stream)));
    assert(!pico_controller_stream_begin(NULL, &config));
    assert(!pico_controller_stream_begin(&stream, NULL));
    assert(!pico_controller_stream_report(NULL, 1, 1024, 1, 2, anchor, payload[0], 104, 1000000400, &event));
    assert(!pico_controller_stream_report(&stream, stream.generation, 1024, 1, 2, anchor, payload[0], 104, 1000000400, NULL));
    pico_controller_stream_invalidate(NULL);
    printf("Pico controller stream PASS: %u rejections and 1000 reconnect generations\n", rejected);
    return 0;
}
