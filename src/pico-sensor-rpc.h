#ifndef REFRAMED_PICO_SENSOR_RPC_H
#define REFRAMED_PICO_SENSOR_RPC_H

#include "sns-low-latency.h"

enum pico_sensor_rpc_state {
    PICO_SENSOR_RPC_CLOSED,
    PICO_SENSOR_RPC_OPEN,
    PICO_SENSOR_RPC_UNCERTAIN,
    PICO_SENSOR_RPC_READY,
    PICO_SENSOR_RPC_ACTIVE,
};

/* Zero-initialize and keep on one thread. The RPC handle is owned; the ION
 * descriptor, mapping and registration are borrowed and must outlive close.
 * Do not copy an open client or unload its FastRPC library. */
struct pico_sensor_rpc {
    uint64_t handle;
    int32_t channel;
    enum pico_sensor_rpc_state state;
};

int pico_sensor_rpc_open(struct pico_sensor_rpc *client);
int pico_sensor_rpc_init(struct pico_sensor_rpc *client, int32_t fd, uint32_t size);
int pico_sensor_rpc_config(struct pico_sensor_rpc *client, int64_t offset_ns,
                           const struct sns_low_latency_suid *suid, int32_t period_us,
                           uint32_t flags, int32_t sensor_handle);
int pico_sensor_rpc_stop(struct pico_sensor_rpc *client);

/* Stops, deinitializes and closes in order. Errors retain the last confirmed
 * state; no operation is retried within a call. An uncertain init cannot be
 * closed here: retain its buffer until DSP teardown is independently confirmed.
 * Only successful close permits releasing the borrowed buffer unconditionally. */
int pico_sensor_rpc_close(struct pico_sensor_rpc *client);

#endif
