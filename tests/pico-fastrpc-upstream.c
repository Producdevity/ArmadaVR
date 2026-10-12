#define _GNU_SOURCE
#ifdef NDEBUG
#error "FastRPC test assertions must be enabled"
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

static int capture_ioctl(int fd, unsigned long request, ...);
#define ioctl capture_ioctl
#include "fastrpc_ioctl.c"
#include <pico-fastrpc.c>
#undef ioctl

static unsigned calls;
static unsigned long expected_request;
static int result, error;
static uint32_t expected_attr, expected_control, expected_value;

static int capture_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 7 && request == expected_request);
    calls++;
    va_list ap;
    va_start(ap, request);
    if ((request & 255) == 5) {
        assert(va_arg(ap, unsigned long) == expected_value);
    } else {
        void *data = va_arg(ap, void *);
        switch (request & 255) {
        case 1:
        case 4:
        case 7:
        case 11: {
            struct vendor_invoke_crc *call = data;
            assert(call->invoke.invoke.invoke.scalar == 0x10000);
            assert(call->invoke.invoke.invoke.handle == 17);
            assert(call->invoke.invoke.invoke.args[0].pointer == 0x1000);
            assert(call->invoke.invoke.invoke.args[0].length == 4096);
            if ((request & 255) == 7)
                assert(call->invoke.attrs[0] == expected_attr);
            break;
        }
        case 6: {
            struct vendor_init *call = data;
            assert(call->flags == 3 && call->file_fd == -1 && !call->memory);
            break;
        }
        case 8:
            assert(*(uint32_t *)data == 2);
            *(uint32_t *)data = 1;
            break;
        case 12: {
            uint32_t *call = data;
            assert(call[0] == expected_control);
            if (call[0] == 3)
                call[1] = 1;
            else
                assert(call[1] == expected_value);
            break;
        }
        case 13:
            assert(*(int32_t *)data == 9);
            break;
        case 14: {
            struct vendor_map *call = data;
            assert(call->fd == 9 && !call->flags && call->address == 0x1000 && call->size == 4096);
            call->dsp_address = 0x12345000;
            break;
        }
        case 15: {
            struct vendor_unmap *call = data;
            assert(call->dsp_address == 0x12345000 && call->size == 4096);
            break;
        }
        case 16: {
            uint32_t *call = data;
            assert(call[0] == 2);
            for (unsigned i = 0; i < 7; i++)
                call[i + 1] = i + 10;
            break;
        }
        default:
            assert(0);
        }
    }
    va_end(ap);
    errno = error;
    return result;
}

int main(void)
{
    assert(sizeof(void *) == 8);
    struct fastrpc_invoke_args arg = {.ptr = 0x1000, .length = 4096, .fd = -1};
    const uint32_t attributes[] = {0, 2, 4, 8, 16, 32, 64, 256, 256 | 2 | 8, 0x17e};
    for (unsigned i = 0; i < sizeof(attributes) / sizeof(*attributes); i++) {
        arg.attr = attributes[i];
        expected_attr = (arg.attr & ~256U) | ((arg.attr & 256) ? 1 : 0);
        expected_request = arg.attr ? 0xc0205207UL : 0xc0105201UL;
        assert(ioctl_invoke(7, INVOKE_ATTRS, 17, 0x10000, &arg, NULL, NULL, NULL, NULL, NULL) == 0);
        assert(arg.attr == attributes[i]);
    }
    unsigned before = calls;
    const uint32_t rejected[] = {1, 128, 512, UINT32_MAX};
    for (unsigned i = 0; i < 4; i++) {
        arg.attr = rejected[i];
        assert(ioctl_invoke(7, INVOKE_ATTRS, 17, 0x10000, &arg, NULL, NULL, NULL, NULL, NULL) ==
               -1);
        assert(errno == EINVAL);
    }
    assert(ioctl_invoke(7, INVOKE_PERF, 17, 0x10000, &arg, NULL, NULL, NULL, NULL, NULL) == -1);
    assert(errno == ENOTTY && calls == before);
    arg.attr = 0;
    expected_request = 0xc0105201;
    result = -1;
    error = EINTR;
    assert(ioctl_invoke(7, INVOKE, 17, 0x10000, &arg, NULL, NULL, NULL, NULL, NULL) == -1);
    assert(errno == EINTR && calls == before + 1);
    result = 57;
    assert(ioctl_invoke(7, INVOKE, 17, 0x10000, &arg, NULL, NULL, NULL, NULL, NULL) == 57);
    result = 0;

    expected_request = 0xc0285206;
    assert(ioctl_init(7, 3, 0, NULL, 0, -1, NULL, 0, 0, 0) == 0);
    before = calls;
    assert(ioctl_init(7, 0, 0, NULL, 0, -1, NULL, 0, 0, 0) == -1);
    assert(errno == EOPNOTSUPP && calls == before);
    result = -1;
    error = EINVAL;
    assert(ioctl_init(7, 3, 0, NULL, 0, -1, NULL, 0, 0, 0) == -1 && errno == EINVAL);
    result = 0;
    uint32_t info = 2;
    expected_request = 0xc0045208;
    assert(ioctl_getinfo(7, &info) == 0 && info == 1);
    expected_request = 0xc0045205;
    expected_value = 4;
    assert(ioctl_setmode(7, 4) == 0);

    uint64_t address = 99;
    expected_request = 0xc020520e;
    assert(ioctl_mmap(7, MEM_MAP, 0, 0, 9, 0, 4096, 0x1000, &address) == 0);
    assert(address == 0x12345000);
    before = calls;
    assert(ioctl_mmap(7, MEM_MAP, FASTRPC_MAP_FD, 0, 9, 0, 4096, 0x1000, &address) == -1);
    assert(errno == ENOTTY && calls == before);
    result = 53;
    address = 99;
    assert(ioctl_mmap(7, MMAP_64, 0, 0, 9, 0, 4096, 0x1000, &address) == 53 && address == 99);
    result = 0;
    expected_request = 0xc010520f;
    assert(ioctl_munmap(7, MEM_UNMAP, 0, NULL, 9, 4096, 0x12345000) == 0);
    expected_request = 0xc018520d;
    assert(ioctl_munmap(7, MUNMAP_FD, FASTRPC_ATTR_KEEP_MAP, (void *)0x1000, 9, 4096, 0) == 0);

    expected_request = 0xc0205210;
    uint32_t capability = 99;
    assert(ioctl_getdspinfo(7, 2, 6, &capability) == 0 && capability == 16);
    before = calls;
    assert(ioctl_getdspinfo(7, 2, 7, &capability) == 0 && !capability && calls == before);
    expected_request = 0xc00c520c;
    struct fastrpc_ctrl_smmu smmu = {.sharedcb = 1};
    assert(ioctl_control(7, DSPRPC_SMMU_SUPPORT, &smmu) == -1 && errno == ENOTTY &&
           calls == before);
    expected_control = 3;
    struct fastrpc_ctrl_kalloc allocation = {0};
    assert(ioctl_control(7, DSPRPC_KALLOC_SUPPORT, &allocation) == 0 &&
           allocation.kalloc_support == 1);
    expected_control = 1;
    expected_value = 1;
    struct fastrpc_ctrl_latency latency = {.enable = 1, .latency = 500};
    assert(ioctl_control(7, DSPRPC_CONTROL_LATENCY, &latency) == 0);
    expected_control = 4;
    struct fastrpc_ctrl_wakelock wake = {.enable = 1};
    assert(ioctl_control(7, DSPRPC_CONTROL_WAKELOCK, &wake) == 0);
    expected_control = 5;
    expected_value = 500;
    struct fastrpc_ctrl_pm pm = {.timeout = 500};
    assert(ioctl_control(7, DSPRPC_PM, &pm) == 0);
    puts("PICO_FASTRPC_UPSTREAM_PASS");
    return 0;
}
