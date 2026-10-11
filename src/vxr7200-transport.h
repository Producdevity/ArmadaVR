#ifndef ARMADA_VXR7200_TRANSPORT_H
#define ARMADA_VXR7200_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

struct vxr7200_bus {
    void *context;
    /* One write message, optionally followed by a read in the same transaction.
     * Return the completed message count or a negative errno. The adapter owns
     * the I2C address, locking and timeout; it must not retain these buffers. */
    int (*transfer)(void *context, const uint8_t *tx, size_t tx_size,
                    uint8_t *rx, size_t rx_size);
};

/* One attempt per call. Callers decide whether a failed write can be retried.
 * Reads leave value unchanged on failure. No GPIO, power or firmware commands
 * are issued implicitly. Return zero on success or a negative errno. */
int vxr7200_read32(const struct vxr7200_bus *bus, uint32_t address, uint32_t *value);
int vxr7200_write32(const struct vxr7200_bus *bus, uint32_t address, uint32_t value);
int vxr7200_read_chip_id(const struct vxr7200_bus *bus, uint16_t *chip_id);

#endif
