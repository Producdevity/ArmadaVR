#include "vxr7200-transport.h"

#include <errno.h>

static void put_le32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = (uint8_t)(value >> (i * 8));
}

int vxr7200_read32(const struct vxr7200_bus *bus, uint32_t address, uint32_t *value)
{
    if (!bus || !bus->transfer || !value)
        return -EINVAL;
    uint8_t request[4], response[5] = {0};
    put_le32(request, address);
    int result = bus->transfer(bus->context, request, sizeof(request), response, sizeof(response));
    if (result != 2)
        return result < 0 ? result : -EIO;
    /* The stock bridge reads five bytes but uses only the first four. */
    uint32_t decoded = 0;
    for (unsigned i = 0; i < 4; ++i)
        decoded |= (uint32_t)response[i] << (i * 8);
    *value = decoded;
    return 0;
}

int vxr7200_write32(const struct vxr7200_bus *bus, uint32_t address, uint32_t value)
{
    if (!bus || !bus->transfer)
        return -EINVAL;
    uint8_t request[8];
    put_le32(request, address);
    put_le32(request + 4, value);
    int result = bus->transfer(bus->context, request, sizeof(request), NULL, 0);
    return result == 1 ? 0 : result < 0 ? result : -EIO;
}

int vxr7200_read_chip_id(const struct vxr7200_bus *bus, uint16_t *chip_id)
{
    if (!bus || !bus->transfer || !chip_id)
        return -EINVAL;
    uint8_t request[4], response[4] = {0};
    put_le32(request, 0x200d38);
    int result = bus->transfer(bus->context, request, sizeof(request), response, sizeof(response));
    if (result != 2)
        return result < 0 ? result : -EIO;
    *chip_id = (uint16_t)((uint16_t)response[2] | (uint16_t)response[3] << 8);
    return 0;
}
