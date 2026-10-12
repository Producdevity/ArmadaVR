#include "pico-sensor-rpc.h"
#include "remote.h"

#include <errno.h>
#include <limits.h>

static int invoke(void *context, uint32_t scalar, const void *input, size_t input_size,
                  void *output, size_t output_size) {
    struct pico_sensor_rpc *client = context;
    remote_arg args[2] = {0};
    args[0].buf.pv = (void *)input;
    args[0].buf.nLen = input_size;
    args[1].buf.pv = output;
    args[1].buf.nLen = output_size;
    if (scalar == 0x00010100)
        client->state = PICO_SENSOR_RPC_UNCERTAIN;
    else if (scalar == 0x01010000)
        client->state = PICO_SENSOR_RPC_ACTIVE;
    return remote_handle64_invoke(client->handle, scalar, args);
}

int pico_sensor_rpc_open(struct pico_sensor_rpc *client) {
    if (!client)
        return -EINVAL;
    if (client->state != PICO_SENSOR_RPC_CLOSED)
        return -EBUSY;
    remote_handle64 mode = 0, handle = 0;
    int result =
        remote_handle64_open(ITRANSPORT_PREFIX "createstaticpd:sensorspd" SDSP_DOMAIN, &mode);
    if (result)
        return result;
    result = remote_handle64_open("sns_low_lat_stream" SDSP_DOMAIN, &handle);
    if (result)
        return result;
    *client = (struct pico_sensor_rpc){handle, -1, PICO_SENSOR_RPC_OPEN};
    return 0;
}

int pico_sensor_rpc_init(struct pico_sensor_rpc *client, int32_t fd, uint32_t size) {
    if (!client || client->state != PICO_SENSOR_RPC_OPEN)
        return -EINVAL;
    int result = sns_low_latency_init(invoke, client, fd, size, &client->channel);
    if (!result)
        client->state = PICO_SENSOR_RPC_READY;
    return result;
}

int pico_sensor_rpc_config(struct pico_sensor_rpc *client, int64_t offset_ns,
                           const struct sns_low_latency_suid *suid, int32_t period_us,
                           uint32_t flags, int32_t sensor_handle) {
    if (!client ||
        (client->state != PICO_SENSOR_RPC_READY && client->state != PICO_SENSOR_RPC_ACTIVE))
        return -EINVAL;
    return sns_low_latency_config(invoke, client, client->channel, offset_ns, suid, period_us,
                                  flags, sensor_handle);
}

int pico_sensor_rpc_stop(struct pico_sensor_rpc *client) {
    if (!client)
        return -EINVAL;
    if (client->state == PICO_SENSOR_RPC_READY)
        return 0;
    if (client->state != PICO_SENSOR_RPC_ACTIVE)
        return -EINVAL;
    int result = sns_low_latency_stop(invoke, client, client->channel);
    if (!result)
        client->state = PICO_SENSOR_RPC_READY;
    return result;
}

int pico_sensor_rpc_close(struct pico_sensor_rpc *client) {
    if (!client)
        return -EINVAL;
    switch (client->state) {
    case PICO_SENSOR_RPC_CLOSED:
        return 0;
    case PICO_SENSOR_RPC_UNCERTAIN:
        return -EBUSY;
    case PICO_SENSOR_RPC_ACTIVE: {
        int result = pico_sensor_rpc_stop(client);
        if (result)
            return result;
        break;
    }
    case PICO_SENSOR_RPC_READY:
    case PICO_SENSOR_RPC_OPEN:
        break;
    default:
        return -EINVAL;
    }
    if (client->state == PICO_SENSOR_RPC_READY) {
        int result = sns_low_latency_deinit(invoke, client, client->channel);
        if (result)
            return result;
        client->state = PICO_SENSOR_RPC_OPEN;
        client->channel = -1;
    }
    int result = remote_handle64_close(client->handle);
    if (!result)
        *client = (struct pico_sensor_rpc){0};
    return result;
}
