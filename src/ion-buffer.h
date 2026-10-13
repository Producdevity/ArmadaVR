#ifndef REFRAMED_ION_BUFFER_H
#define REFRAMED_ION_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ion_buffer {
    int fd;
    void *data;
    size_t size;
};
enum ion_buffer_access { ION_BUFFER_READ = 1, ION_BUFFER_WRITE = 2, ION_BUFFER_READ_WRITE = 3 };

/* Pico's ION v2 ABI and 4 KiB pages. Initialize the output with {.fd = -1}.
 * The caller owns ion_fd and selects a verified heap. Success owns a new
 * CLOEXEC DMA-BUF and mapping; size is rounded up. Failure preserves output.
 * Buffers are thread-confined and must not be copied while owned. */
int ion_buffer_allocate(int ion_fd, uint32_t heap_id, size_t size, bool cached,
                        struct ion_buffer *buffer);

/* Bracket each CPU access, including initial ring clearing, with start/end.
 * Return values are zero or negative errno; interrupted calls are not retried.
 * CPU synchronization does not establish DSP mapping or device coherency. */
int ion_buffer_sync(const struct ion_buffer *buffer, enum ion_buffer_access access, bool end);

/* The DSP must have released the buffer before this call. A failed munmap
 * preserves ownership. Once unmapped, close is attempted once and ownership
 * is cleared even if close reports EINTR, as required by Linux FD semantics. */
int ion_buffer_release(struct ion_buffer *buffer);

#endif
