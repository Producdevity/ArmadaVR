#ifdef NDEBUG
#error "Sensor RPC assertions must be enabled"
#endif
#include "pico-sensor-rpc.h"
#include "remote.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int mode_error, open_error, invoke_error, close_error;
static uint32_t reply = 23;
static char calls[128];
static size_t count;
static void record(char operation) {
    assert(count + 1 < sizeof(calls));
    calls[count++] = operation;
    calls[count] = 0;
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int remote_handle64_open(const char *name, remote_handle64 *handle) {
    if (!strcmp(name, ITRANSPORT_PREFIX "createstaticpd:sensorspd" SDSP_DOMAIN)) {
        record('M');
        if (!mode_error)
            *handle = 258;
        return mode_error;
    }
    assert(!strcmp(name, "sns_low_lat_stream" SDSP_DOMAIN));
    record('O');
    if (!open_error)
        *handle = UINT64_C(0x100000001);
    return open_error;
}
int remote_handle64_close(remote_handle64 handle) {
    assert(handle == UINT64_C(0x100000001));
    record('C');
    return close_error;
}
int remote_handle64_invoke(remote_handle64 handle, uint32_t scalar, remote_arg *args) {
    assert(handle == UINT64_C(0x100000001));
    const unsigned char *input = args[0].buf.pv;
    assert(input);
    switch (scalar) {
    case 0x00010100:
        record('I');
        assert(args[0].buf.nLen == 12 && args[1].buf.nLen == 4 && args[1].buf.pv);
        assert(get32(input) == 0 && get32(input + 4) == 8192 && get32(input + 8) == 0);
        for (unsigned i = 0; i < 4; i++)
            ((unsigned char *)args[1].buf.pv)[i] = (unsigned char)(reply >> (8 * i));
        break;
    case 0x01010000:
        record('F');
        assert(args[0].buf.nLen == 48 && get32(input) == 23);
        assert(!get32(input + 4) && !get32(input + 44));
        assert(get32(input + 8) == UINT32_MAX - 42 && get32(input + 12) == UINT32_MAX);
        assert(get32(input + 16) == 7 && get32(input + 24) == 8);
        assert(get32(input + 32) == 1000 && get32(input + 36) == 5 && get32(input + 40) == 9);
        assert(!args[1].buf.pv && !args[1].buf.nLen);
        break;
    case 0x02010000:
    case 0x03010000:
        record(scalar == 0x02010000 ? 'S' : 'D');
        assert(args[0].buf.nLen == 4 && get32(input) == 23);
        assert(!args[1].buf.pv && !args[1].buf.nLen);
        break;
    default:
        assert(0);
    }
    return invoke_error;
}
static void reset(void) {
    count = 0;
    calls[0] = 0;
    mode_error = open_error = invoke_error = close_error = 0;
    reply = 23;
}
static struct pico_sensor_rpc ready(void) {
    struct pico_sensor_rpc client = {0};
    assert(!pico_sensor_rpc_open(&client));
    assert(!pico_sensor_rpc_init(&client, 0, 8192));
    assert(client.state == PICO_SENSOR_RPC_READY && client.channel == 23);
    return client;
}
static int configure(struct pico_sensor_rpc *client) {
    const struct sns_low_latency_suid suid = {7, 8};
    return pico_sensor_rpc_config(client, -43, &suid, 1000, 5, 9);
}
int main(void) {
    struct pico_sensor_rpc client = {0}, before;
    assert(pico_sensor_rpc_open(NULL) == -EINVAL);
    assert(pico_sensor_rpc_close(NULL) == -EINVAL);
    assert(pico_sensor_rpc_stop(NULL) == -EINVAL);
    assert(pico_sensor_rpc_init(NULL, 0, 8192) == -EINVAL);
    assert(configure(NULL) == -EINVAL);
    assert(!pico_sensor_rpc_close(&client) && !count);
    for (unsigned cycle = 0; cycle < 128; cycle++) {
        reset();
        client = ready();
        assert(!pico_sensor_rpc_stop(&client));
        assert(!configure(&client) && !configure(&client));
        assert(!pico_sensor_rpc_close(&client));
        assert(client.state == PICO_SENSOR_RPC_CLOSED);
        assert(!strcmp(calls, "MOIFFSDC"));
        assert(!pico_sensor_rpc_close(&client));
    }
    const int errors[] = {57, -EINTR, -EIO};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(*errors); i++) {
        int error = errors[i];
        reset();
        client = (struct pico_sensor_rpc){0};
        before = client;
        mode_error = error;
        assert(pico_sensor_rpc_open(&client) == error && !memcmp(&client, &before, sizeof(client)));
        assert(!strcmp(calls, "M"));
        reset();
        open_error = error;
        assert(pico_sensor_rpc_open(&client) == error && !memcmp(&client, &before, sizeof(client)));
        assert(!strcmp(calls, "MO"));
        reset();
        assert(!pico_sensor_rpc_open(&client));
        invoke_error = error;
        assert(pico_sensor_rpc_init(&client, 0, 8192) == error);
        assert(client.state == PICO_SENSOR_RPC_UNCERTAIN && client.channel == -1);
        assert(pico_sensor_rpc_close(&client) == -EBUSY);
        assert(pico_sensor_rpc_open(&client) == -EBUSY);
        assert(pico_sensor_rpc_init(&client, 0, 8192) == -EINVAL);
        assert(configure(&client) == -EINVAL && pico_sensor_rpc_stop(&client) == -EINVAL);
        assert(!strcmp(calls, "MOI"));
        reset();
        client = ready();
        invoke_error = error;
        assert(configure(&client) == error && client.state == PICO_SENSOR_RPC_ACTIVE);
        assert(pico_sensor_rpc_close(&client) == error && client.state == PICO_SENSOR_RPC_ACTIVE);
        assert(!strcmp(calls, "MOIFS"));
        invoke_error = 0;
        assert(!pico_sensor_rpc_stop(&client) && client.state == PICO_SENSOR_RPC_READY);
        invoke_error = error;
        assert(pico_sensor_rpc_close(&client) == error && client.state == PICO_SENSOR_RPC_READY);
        assert(!strcmp(calls, "MOIFSSD"));
        invoke_error = 0;
        close_error = error;
        assert(pico_sensor_rpc_close(&client) == error && client.state == PICO_SENSOR_RPC_OPEN);
        assert(client.channel == -1 && !strcmp(calls, "MOIFSSDDC"));
        close_error = 0;
        assert(!pico_sensor_rpc_close(&client) && !strcmp(calls, "MOIFSSDDCC"));
    }
    reset();
    client = (struct pico_sensor_rpc){0};
    assert(!pico_sensor_rpc_open(&client));
    assert(pico_sensor_rpc_init(&client, -1, 8192) == -EINVAL);
    assert(pico_sensor_rpc_init(&client, 0, 0) == -EINVAL);
    assert(pico_sensor_rpc_init(&client, 0, UINT32_MAX) == -EINVAL);
    assert(client.state == PICO_SENSOR_RPC_OPEN && !strcmp(calls, "MO"));
    assert(!pico_sensor_rpc_close(&client) && !strcmp(calls, "MOC"));
    reset();
    assert(!pico_sensor_rpc_open(&client));
    reply = UINT32_MAX;
    assert(pico_sensor_rpc_init(&client, 0, 8192) == -EPROTO);
    assert(client.state == PICO_SENSOR_RPC_UNCERTAIN && client.channel == -1);
    assert(pico_sensor_rpc_close(&client) == -EBUSY);
    reset();
    client = ready();
    assert(pico_sensor_rpc_config(&client, 0, NULL, 0, 0, 0) == -EINVAL);
    assert(client.state == PICO_SENSOR_RPC_READY && !strcmp(calls, "MOI"));
    assert(!pico_sensor_rpc_close(&client) && !strcmp(calls, "MOIDC"));
    puts("PICO_SENSOR_RPC_PASS cycles=128 error_stages=6 error_codes=3");
    return 0;
}
