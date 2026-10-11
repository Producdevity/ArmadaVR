#include "vxr7200-transport.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fake_bus {
    unsigned calls;
    int result;
    uint8_t request[8], response[5];
    size_t tx_size, rx_size;
};

static int transfer(void *context, const uint8_t *tx, size_t tx_size,
                     uint8_t *rx, size_t rx_size)
{
    struct fake_bus *fake = context;
    assert(tx_size == 4 || tx_size == 8);
    assert((tx_size == 4 && rx && (rx_size == 4 || rx_size == 5)) ||
           (tx_size == 8 && !rx && !rx_size));
    ++fake->calls;
    memcpy(fake->request, tx, tx_size);
    fake->tx_size = tx_size;
    fake->rx_size = rx_size;
    if (rx)
        memcpy(rx, fake->response, rx_size);
    return fake->result;
}

int main(void)
{
    unsigned checks = 0;
    struct fake_bus fake = {0};
    struct vxr7200_bus bus = {&fake, transfer}, missing = {0};
    uint32_t value = 0xdeadbeef;
    assert(vxr7200_read32(NULL, 0, &value) == -EINVAL);
    assert(vxr7200_read32(&missing, 0, &value) == -EINVAL);
    assert(vxr7200_read32(&bus, 0, NULL) == -EINVAL);
    assert(vxr7200_write32(NULL, 0, 0) == -EINVAL);
    assert(vxr7200_write32(&missing, 0, 0) == -EINVAL);
    assert(!fake.calls && value == 0xdeadbeef);
    checks += 5;
    uint16_t chip_id = 0xdead;
    assert(vxr7200_read_chip_id(NULL, &chip_id) == -EINVAL);
    assert(vxr7200_read_chip_id(&missing, &chip_id) == -EINVAL);
    assert(vxr7200_read_chip_id(&bus, NULL) == -EINVAL);
    assert(!fake.calls && chip_id == 0xdead);
    checks += 3;

    const int failures[] = {-EIO, -ENXIO, -ETIMEDOUT, -EAGAIN, -EINTR, 0, 3, 8};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(*failures); ++i) {
        fake.result = failures[i];
        unsigned calls = fake.calls;
        int expected = fake.result < 0 ? fake.result : -EIO;
        assert(vxr7200_read32(&bus, 0x12345678, &value) == expected);
        assert(value == 0xdeadbeef && fake.calls == calls + 1);
        assert(vxr7200_write32(&bus, 0x12345678, value) == expected);
        assert(fake.calls == calls + 2);
        assert(vxr7200_read_chip_id(&bus, &chip_id) == expected);
        assert(fake.calls == calls + 3 && chip_id == 0xdead);
        checks += 3;
    }
    fake.result = 1;
    assert(vxr7200_read32(&bus, 0, &value) == -EIO && value == 0xdeadbeef);
    assert(vxr7200_read_chip_id(&bus, &chip_id) == -EIO && chip_id == 0xdead);
    fake.result = 2;
    assert(vxr7200_write32(&bus, 0, value) == -EIO);
    checks += 3;

    const uint8_t request[] = {0x38, 0x0d, 0x20, 0x00};
    const uint8_t reply[] = {0x12, 0x34, 0x56, 0x78, 0};
    memcpy(fake.response, reply, sizeof(reply));
    for (unsigned trailer = 0; trailer < 256; ++trailer) {
        fake.response[4] = (uint8_t)trailer;
        assert(!vxr7200_read32(&bus, 0x200d38, &value));
        assert(value == 0x78563412 && fake.tx_size == 4 && fake.rx_size == 5);
        assert(!memcmp(fake.request, request, sizeof(request)));
        ++checks;
    }
    assert(!vxr7200_read_chip_id(&bus, &chip_id) && chip_id == 0x7856);
    assert(fake.tx_size == 4 && fake.rx_size == 4);
    assert(!memcmp(fake.request, request, sizeof(request)));
    ++checks;
    fake.result = 1;
    const uint8_t write_request[] = {0xfc, 0x00, 0x20, 0x00, 0xf5, 0x00, 0x00, 0x00};
    assert(!vxr7200_write32(&bus, 0x2000fc, 0xf5));
    assert(!memcmp(fake.request, write_request, sizeof(write_request)));
    assert(fake.tx_size == 8 && !fake.rx_size);
    ++checks;

    uint32_t random = 0x72002026;
    for (unsigned n = 0; n < 4096; ++n) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        uint32_t address = random, expected = ~random;
        assert(!vxr7200_write32(&bus, address, expected));
        for (unsigned byte = 0; byte < 4; ++byte) {
            assert(fake.request[byte] == (uint8_t)(address >> (byte * 8)));
            assert(fake.request[byte + 4] == (uint8_t)(expected >> (byte * 8)));
        }
        memcpy(fake.response, fake.request + 4, 4);
        fake.result = 2;
        assert(!vxr7200_read32(&bus, address, &value) && value == expected);
        assert(!vxr7200_read_chip_id(&bus, &chip_id) && chip_id == expected >> 16);
        fake.result = 1;
        checks += 3;
    }
    printf("%u VXR7200 transport checks passed\n", checks);
    return 0;
}
