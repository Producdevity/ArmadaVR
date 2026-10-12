#ifndef ARMADA_PICO_FASTRPC_H
#define ARMADA_PICO_FASTRPC_H

#include <stddef.h>
#include <stdint.h>

enum pico_fastrpc_attribute {
    PICO_FASTRPC_NOVA = 1,
    PICO_FASTRPC_NON_COHERENT = 2,
    PICO_FASTRPC_COHERENT = 4,
    PICO_FASTRPC_KEEP_MAP = 8,
    PICO_FASTRPC_NOMAP = 16,
    PICO_FASTRPC_NOFLUSH = 32,
    PICO_FASTRPC_NOINVALIDATE = 64,
};

struct pico_fastrpc_arg {
    uint64_t pointer;
    uint64_t length;
    int32_t fd;
    uint32_t attributes;
};

/* The caller owns an authorized adsprpc-smd descriptor and its session.
 * Select the domain before attaching; reopen after a failed selection, since
 * the vendor driver can retain the domain even when GETINFO fails. */
int pico_fastrpc_select_domain(int dev, uint32_t domain, uint32_t *info);
int pico_fastrpc_attach_sensors(int dev);
int pico_fastrpc_set_mode(int dev, uint32_t mode);

/* Map/unmap the borrowed DMA-BUF through the vendor's 64-bit address ioctls.
 * The caller keeps it alive until confirmed unmap or session teardown.
 * Failure preserves the returned DSP address. */
int pico_fastrpc_map(int dev, int fd, uint32_t flags, uintptr_t address, size_t size,
                     uint64_t *dsp_address);
int pico_fastrpc_unmap(int dev, uint64_t dsp_address, size_t size);

/* Arguments use vendor attribute bits, not the mainline NOVA value (256).
 * Buffer arguments contain a CPU address and length; DMA handle arguments
 * contain a 32-bit offset and length plus their FD. The scalar determines
 * the number of each, and count must match. Buffers and FDs remain borrowed.
 * crc, if supplied, points to 64 uint32_t entries for the vendor CRC ioctl.
 * Returns the driver/DSP status or negative errno. Output buffers may change
 * on failure; interrupted calls must not be retried automatically. */
int pico_fastrpc_invoke(int dev, uint32_t handle, uint32_t scalar,
                        const struct pico_fastrpc_arg *args, size_t count, uint32_t *crc);

#endif
