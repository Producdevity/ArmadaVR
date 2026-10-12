#include "../src/pico-fastrpc.h"
#ifdef NDEBUG
#error "FastRPC test assertions must be enabled"
#endif
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

static int mock_ioctl(int fd, unsigned long request, ...);
#define ioctl mock_ioctl
#include "../src/pico-fastrpc.c"
#undef ioctl

static const struct pico_fastrpc_arg *expected;
static size_t expected_count;
static uint32_t expected_scalar, expected_domain, *expected_crc;
static unsigned long expected_request;
static unsigned calls;
static int injected_status, injected_errno;

static uint32_t word(const void *data, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const char *)data + offset, sizeof(value));
    return value;
}

static uintptr_t address(const void *data, size_t offset)
{
    uintptr_t value;
    memcpy(&value, (const char *)data + offset, sizeof(value));
    return value;
}

static int mock_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 7 && request == expected_request);
    calls++;
    va_list ap;
    va_start(ap, request);
    void *data = va_arg(ap, void *);
    va_end(ap);
    size_t width = sizeof(uintptr_t);
    unsigned number = request & 255;
    if (number == 8) {
        assert(word(data, 0) == expected_domain);
        *(uint32_t *)data = 1;
    } else if (number == 6) {
        assert(word(data, 0) == 3);
        size_t file = width == 8 ? 8 : 4;
        assert(address(data, file) == 0);
        assert(word(data, file + width) == 0);
        assert(word(data, file + width + 4) == UINT32_MAX);
        assert(address(data, file + width + 8) == 0);
        assert(word(data, file + 2 * width + 8) == 0);
        assert(word(data, file + 2 * width + 12) == 0);
    } else {
        assert(word(data, 0) == 0x12345678 && word(data, 4) == expected_scalar);
        const void *packed = (const void *)address(data, 8);
        assert((packed != NULL) == (expected_count != 0));
        const void *fd_list = number == 1 ? NULL : (const void *)address(data, 8 + width);
        const void *attr_list =
            number == 7 || number == 11 ? (const void *)address(data, 8 + 2 * width) : NULL;
        for (size_t i = 0; i < expected_count; i++) {
            assert(address(packed, 2 * width * i) == expected[i].pointer);
            assert(address(packed, 2 * width * i + width) == expected[i].length);
            if (fd_list)
                assert(word(fd_list, 4 * i) == (uint32_t)expected[i].fd);
            if (attr_list)
                assert(word(attr_list, 4 * i) == expected[i].attributes);
        }
        if (number == 11) {
            assert((uint32_t *)address(data, 8 + 3 * width) == expected_crc);
            expected_crc[63] = 0xabcdef01;
        }
    }
    errno = injected_errno;
    return injected_status;
}

static void setup_tests(void)
{
    expected_request = 0xc0045208UL;
    for (expected_domain = 0; expected_domain < 4; expected_domain++) {
        uint32_t info = 0xdeadbeef;
        injected_status = -1;
        injected_errno = EACCES;
        assert(pico_fastrpc_select_domain(7, expected_domain, &info) == -EACCES);
        assert(info == 0xdeadbeef);
        injected_status = 0;
        assert(pico_fastrpc_select_domain(7, expected_domain, &info) == 0 && info == 1);
    }
    unsigned before = calls;
    uint32_t info = 99;
    assert(pico_fastrpc_select_domain(7, 4, &info) == -EINVAL);
    assert(pico_fastrpc_select_domain(7, 0, NULL) == -EINVAL);
    assert(pico_fastrpc_select_domain(-1, 0, &info) == -EINVAL);
    assert(pico_fastrpc_attach_sensors(-1) == -EINVAL);
    assert(calls == before && info == 99);
    expected_request = sizeof(uintptr_t) == 8 ? 0xc0285206UL : 0xc01c5206UL;
    injected_status = -1;
    injected_errno = EINVAL;
    assert(pico_fastrpc_attach_sensors(7) == -EINVAL);
    injected_status = 0;
    assert(pico_fastrpc_attach_sensors(7) == 0);
}

static void invoke_tests(void)
{
    const uint32_t scalars[] = {0,         0x10100, 0x1010000, 0x2010000, 0x3010000,
                                0x4020200, 0x112,   0x7010101, UINT32_MAX};
    struct pico_fastrpc_arg args[540], original[540];
    expected = args;
    unsigned cases = 0;
    for (size_t sc = 0; sc < sizeof(scalars) / sizeof(*scalars); sc++) {
        expected_scalar = scalars[sc];
        size_t buffers = ((expected_scalar >> 16) & 255) + ((expected_scalar >> 8) & 255);
        expected_count = buffers + ((expected_scalar >> 4) & 15) + (expected_scalar & 15);
        for (unsigned mode = 0; mode < 4; mode++) {
            for (uint32_t attr = 0; attr < 128; attr++) {
                for (size_t i = 0; i < expected_count; i++) {
                    args[i] = (struct pico_fastrpc_arg){
                        .pointer = i < buffers ? 0x10000 + i * 4096 : i * 16,
                        .length = 4096 + i,
                        .fd = mode || i >= buffers ? (int32_t)i : -1,
                        .attributes = mode >= 2 ? attr : 0,
                    };
                }
                memcpy(original, args, expected_count * sizeof(*args));
                unsigned number = expected_count && (mode || buffers < expected_count) ? 4 : 1;
                if (expected_count && mode >= 2 && attr)
                    number = 7;
                uint32_t crc[64] = {0};
                expected_crc = mode == 3 ? crc : NULL;
                if (expected_crc)
                    number = 11;
                size_t width = sizeof(uintptr_t);
                size_t bytes = 8 + width * (number == 1   ? 1
                                            : number == 4 ? 2
                                            : number == 7 ? 3
                                                          : 4);
                expected_request = 0xc0005200UL | (bytes << 16) | number;
                injected_status = 0;
                unsigned before = calls;
                assert(pico_fastrpc_invoke(7, 0x12345678, expected_scalar, args, expected_count,
                                           expected_crc) == 0);
                assert(calls == before + 1);
                assert(!memcmp(original, args, expected_count * sizeof(*args)));
                if (expected_crc)
                    assert(crc[63] == 0xabcdef01);
                cases++;
            }
        }
    }
    expected_count = 1;
    expected_scalar = 0x10000;
    args[0] = (struct pico_fastrpc_arg){.pointer = 0x1000, .length = 8, .fd = -1};
    expected_request = sizeof(uintptr_t) == 8 ? 0xc0105201UL : 0xc00c5201UL;
    injected_status = -1;
    injected_errno = EINTR;
    unsigned before = calls;
    assert(pico_fastrpc_invoke(7, 0x12345678, expected_scalar, args, 1, NULL) == -EINTR);
    assert(calls == before + 1);
    injected_status = 57;
    assert(pico_fastrpc_invoke(7, 0x12345678, expected_scalar, args, 1, NULL) == 57);
    printf("PICO_FASTRPC_PACKING_PASS bits=%zu cases=%u\n", sizeof(void *) * 8, cases);
}

static void rejection_tests(void)
{
    struct pico_fastrpc_arg arg = {.pointer = 0x1000, .length = 8, .fd = -1};
    unsigned before = calls;
    assert(pico_fastrpc_invoke(-1, 0, 0, NULL, 0, NULL) == -EINVAL);
    assert(pico_fastrpc_invoke(7, 0, 0x10000, NULL, 1, NULL) == -EINVAL);
    assert(pico_fastrpc_invoke(7, 0, 0x10000, &arg, 540, NULL) == -EINVAL);
    assert(pico_fastrpc_invoke(7, 0, 0x10000, &arg, 0, NULL) == -EINVAL);
    arg.fd = -2;
    assert(pico_fastrpc_invoke(7, 0, 0x10000, &arg, 1, NULL) == -EINVAL);
    arg.fd = -1;
    arg.attributes = 256;
    assert(pico_fastrpc_invoke(7, 0, 0x10000, &arg, 1, NULL) == -EINVAL);
    arg.attributes = 0;
    arg.pointer = UINTPTR_MAX;
    assert(pico_fastrpc_invoke(7, 0, 0x10000, &arg, 1, NULL) == -EOVERFLOW);
    arg.pointer = 0;
    arg.length = UINT64_MAX;
    assert(pico_fastrpc_invoke(7, 0, 0x10000, &arg, 1, NULL) == -EOVERFLOW);
    arg.length = 8;
    assert(pico_fastrpc_invoke(7, 0, 0x10, &arg, 1, NULL) == -EINVAL);
    arg.fd = 0;
    arg.pointer = UINT64_C(0x100000000);
    assert(pico_fastrpc_invoke(7, 0, 0x10, &arg, 1, NULL) ==
           (sizeof(void *) == 8 ? -EINVAL : -EOVERFLOW));
    arg.pointer = 0;
    arg.length = UINT64_C(0x100000000);
    assert(pico_fastrpc_invoke(7, 0, 0x10, &arg, 1, NULL) ==
           (sizeof(void *) == 8 ? -EINVAL : -EOVERFLOW));
    assert(calls == before);
}

int main(void)
{
    setup_tests();
    invoke_tests();
    rejection_tests();
    puts("PICO_FASTRPC_UNIT_PASS");
    return 0;
}
