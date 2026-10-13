#ifndef REFRAMED_SNS_LOW_LATENCY_H
#define REFRAMED_SNS_LOW_LATENCY_H

#include <stddef.h>
#include <stdint.h>

struct sns_low_latency_suid { uint64_t low, high; };

/* Synchronous transport supplied by the caller. Buffers are little-endian wire
 * data valid only for the call. Return the remote status, zero on success.
 * The transport owns the remote handle, memory mapping and DSP connection. */
typedef int (*sns_low_latency_invoke)(void *context, uint32_t scalar,
                                     const void *input, size_t input_size,
                                     void *output, size_t output_size);

/* b1977 sensor stub ABI. Initialization uses the stock zero third argument.
 * These functions only marshal requests; they open no device and own no channel.
 * Failure leaves channel unchanged. Caller retains the mapped buffer until
 * successful deinitialization or confirmed transport teardown. */
int sns_low_latency_init(sns_low_latency_invoke invoke, void *context,
                         int32_t fd, uint32_t size, int32_t *channel);

/* Supply an explicitly selected SUID, sample period, vendor flags and sensor
 * handle. Offset is the transport's requested timestamp offset in nanoseconds;
 * receipt of that offset in output samples still requires verification. */
int sns_low_latency_config(sns_low_latency_invoke invoke, void *context,
                           int32_t channel, int64_t offset_ns,
                           const struct sns_low_latency_suid *suid,
                           int32_t period_us, uint32_t flags, int32_t sensor_handle);
int sns_low_latency_stop(sns_low_latency_invoke invoke, void *context, int32_t channel);
int sns_low_latency_deinit(sns_low_latency_invoke invoke, void *context, int32_t channel);

#endif
