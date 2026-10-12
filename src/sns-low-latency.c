#include "sns-low-latency.h"
#include <errno.h>
#include <limits.h>

static void put32(unsigned char *data, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++)
        data[i] = (unsigned char)(value >> (8 * i));
}

static void put64(unsigned char *data, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++)
        data[i] = (unsigned char)(value >> (8 * i));
}

int sns_low_latency_init(sns_low_latency_invoke invoke, void *context,
                         int32_t fd, uint32_t size, int32_t *channel)
{
    if (!invoke || !channel || fd < 0 || !size || size > INT32_MAX)
        return -EINVAL;
    unsigned char input[12] = {0}, output[4] = {255, 255, 255, 255};
    put32(input, (uint32_t)fd);
    put32(input + 4, size);
    int result = invoke(context, 0x00010100, input, sizeof(input), output, sizeof(output));
    if (result)
        return result;
    uint32_t value = (uint32_t)output[0] | (uint32_t)output[1] << 8 |
                     (uint32_t)output[2] << 16 | (uint32_t)output[3] << 24;
    if (value > INT32_MAX)
        return -EPROTO;
    *channel = (int32_t)value;
    return 0;
}

int sns_low_latency_config(sns_low_latency_invoke invoke, void *context,
                           int32_t channel, int64_t offset_ns,
                           const struct sns_low_latency_suid *suid,
                           int32_t period_us, uint32_t flags, int32_t sensor_handle)
{
    if (!invoke || !suid || channel < 0 || period_us <= 0 || sensor_handle < 0 ||
        (!suid->low && !suid->high))
        return -EINVAL;
    unsigned char input[48] = {0};
    put32(input, (uint32_t)channel);
    put64(input + 8, (uint64_t)offset_ns);
    put64(input + 16, suid->low);
    put64(input + 24, suid->high);
    put32(input + 32, (uint32_t)period_us);
    put32(input + 36, flags);
    put32(input + 40, (uint32_t)sensor_handle);
    return invoke(context, 0x01010000, input, sizeof(input), NULL, 0);
}

static int channel_request(sns_low_latency_invoke invoke, void *context,
                           int32_t channel, uint32_t scalar)
{
    if (!invoke || channel < 0)
        return -EINVAL;
    unsigned char input[4];
    put32(input, (uint32_t)channel);
    return invoke(context, scalar, input, sizeof(input), NULL, 0);
}

int sns_low_latency_stop(sns_low_latency_invoke invoke, void *context, int32_t channel)
{
    return channel_request(invoke, context, channel, 0x02010000);
}

int sns_low_latency_deinit(sns_low_latency_invoke invoke, void *context, int32_t channel)
{
    return channel_request(invoke, context, channel, 0x03010000);
}
