#include "sns-low-latency.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct transport {
    unsigned calls;
    int result;
    uint32_t response;
};

static int invoke(void *context, uint32_t scalar, const void *input, size_t size,
                  void *output, size_t output_size)
{
    struct transport *transport = context;
    transport->calls++;
    assert(size == 4 || size == 12 || size == 48);
    assert((scalar == 0x10100) == (output_size == 4));
    assert((output != NULL) == (output_size != 0));
    if (size == 48) {
        const unsigned char *bytes = input;
        for (unsigned i = 4; i < 8; i++) assert(bytes[i] == 0 && bytes[i + 40] == 0);
    }
    if (output) {
        for (unsigned i = 0; i < 4; i++)
            ((unsigned char *)output)[i] = transport->response >> (8 * i);
    }
    return transport->result;
}

static int wire(void *context, uint32_t scalar, const void *input, size_t size,
                void *output, size_t output_size)
{
    struct transport *transport = context;
    printf("WIRE %08" PRIx32 " %zu %zu ", scalar, size, output_size);
    for (size_t i = 0; i < size; i++) printf("%02x", ((const unsigned char *)input)[i]);
    puts("");
    return invoke(transport, scalar, input, size, output, output_size);
}

int main(int argc, char **argv)
{
    struct transport transport = {.response = 7};
    struct sns_low_latency_suid suid = {0x1122334455667788, 0x99aabbccddeeff00};
    int32_t channel = 123;
    if (argc == 3) {
        char *end;
        unsigned long n = strtoul(argv[1], &end, 10);
        assert(!*end && n < 32);
        long status = strtol(argv[2], &end, 10);
        assert(!*end && (status == 0 || status == 57 || status == -5));
        transport.result = (int)status;
        suid.low ^= n;
        suid.high ^= (uint64_t)n << 32;
        int64_t offset = n & 1 ? INT64_MIN + (int64_t)n : INT64_MAX - (int64_t)n;
        assert(sns_low_latency_init(wire, &transport, (int32_t)n + 1, 104 * (n + 1), &channel) == status);
        assert(channel == (status ? 123 : 7));
        assert(sns_low_latency_config(wire, &transport, n, offset, &suid, 125 + n,
                                       UINT32_MAX - n, 161 + n) == status);
        assert(sns_low_latency_stop(wire, &transport, n) == status);
        assert(sns_low_latency_deinit(wire, &transport, n) == status);
        return 0;
    }
    assert(argc == 1);
    for (unsigned i = 0; i < 1000; i++) {
        transport.result = i & 1 ? 57 : -5;
        assert(sns_low_latency_init(invoke, &transport, 41, 851968, &channel) == transport.result);
        assert(channel == 123);
        assert(sns_low_latency_config(invoke, &transport, 7, -3337892297, &suid, 1000, 3, 351) == transport.result);
        assert(sns_low_latency_stop(invoke, &transport, 7) == transport.result);
        assert(sns_low_latency_deinit(invoke, &transport, 7) == transport.result);
    }
    transport.result = 0;
    transport.response = UINT32_MAX;
    assert(sns_low_latency_init(invoke, &transport, 0, 104, &channel) == -EPROTO && channel == 123);
    transport.response = 0;
    assert(sns_low_latency_init(invoke, &transport, 0, 104, &channel) == 0 && channel == 0);
    unsigned calls = transport.calls;
    assert(sns_low_latency_init(NULL, &transport, 0, 104, &channel) == -EINVAL);
    assert(sns_low_latency_init(invoke, &transport, -1, 104, &channel) == -EINVAL);
    assert(sns_low_latency_init(invoke, &transport, 0, UINT32_MAX, &channel) == -EINVAL);
    assert(sns_low_latency_init(invoke, &transport, 0, 0, &channel) == -EINVAL);
    assert(sns_low_latency_init(invoke, &transport, 0, 104, NULL) == -EINVAL);
    assert(sns_low_latency_config(invoke, &transport, -1, 0, &suid, 1000, 0, 1) == -EINVAL);
    assert(sns_low_latency_config(invoke, &transport, 0, 0, NULL, 1000, 0, 1) == -EINVAL);
    assert(sns_low_latency_config(invoke, &transport, 0, 0, &suid, 0, 0, 1) == -EINVAL);
    assert(sns_low_latency_config(invoke, &transport, 0, 0, &suid, 1000, 0, -1) == -EINVAL);
    suid = (struct sns_low_latency_suid){0};
    assert(sns_low_latency_config(invoke, &transport, 0, 0, &suid, 1000, 0, 1) == -EINVAL);
    assert(sns_low_latency_stop(invoke, &transport, -1) == -EINVAL);
    assert(sns_low_latency_deinit(NULL, &transport, 0) == -EINVAL);
    assert(transport.calls == calls);
    printf("SNS_LOW_LATENCY_PASS calls=%u\n", calls);
    return 0;
}
