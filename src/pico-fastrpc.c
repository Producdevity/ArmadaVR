#include "pico-fastrpc.h"

#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>

struct vendor_arg {
    uintptr_t pointer;
    size_t length;
};
struct vendor_invoke {
    uint32_t handle, scalar;
    struct vendor_arg *args;
};
struct vendor_invoke_fd {
    struct vendor_invoke invoke;
    int32_t *fds;
};
struct vendor_invoke_attrs {
    struct vendor_invoke_fd invoke;
    uint32_t *attrs;
};
struct vendor_invoke_crc {
    struct vendor_invoke_attrs invoke;
    uint32_t *crc;
};
struct vendor_init {
    uint32_t flags;
    uintptr_t file;
    uint32_t file_length;
    int32_t file_fd;
    uintptr_t memory;
    uint32_t memory_length;
    int32_t memory_fd;
};

_Static_assert(sizeof(uintptr_t) == 4 || sizeof(uintptr_t) == 8, "Pico Linux ABI");
_Static_assert(sizeof(struct vendor_arg) == 2 * sizeof(uintptr_t), "FastRPC argument ABI");
_Static_assert(sizeof(struct vendor_invoke) == 8 + sizeof(uintptr_t), "FastRPC invoke ABI");
_Static_assert(sizeof(struct vendor_init) == (sizeof(uintptr_t) == 8 ? 40 : 28),
               "FastRPC init ABI");

#define VENDOR_REQUEST(number, type) (0xc0005200UL | (sizeof(type) << 16) | (number))
#define MAX_ARGS (255 + 255 + 15 + 15)

int pico_fastrpc_select_domain(int dev, uint32_t domain, uint32_t *info)
{
    if (dev < 0 || domain >= 4 || !info)
        return -EINVAL;
    uint32_t value = domain;
    int result = ioctl(dev, VENDOR_REQUEST(8, uint32_t), &value);
    if (result < 0)
        return -errno;
    if (result)
        return result;
    *info = value;
    return 0;
}

int pico_fastrpc_attach_sensors(int dev)
{
    if (dev < 0)
        return -EINVAL;
    struct vendor_init request = {.flags = 3, .file_fd = -1};
    int result = ioctl(dev, VENDOR_REQUEST(6, struct vendor_init), &request);
    return result < 0 ? -errno : result;
}

int pico_fastrpc_invoke(int dev, uint32_t handle, uint32_t scalar,
                        const struct pico_fastrpc_arg *args, size_t count, uint32_t *crc)
{
    size_t buffers = ((scalar >> 16) & 255) + ((scalar >> 8) & 255);
    size_t handles = ((scalar >> 4) & 15) + (scalar & 15);
    if (dev < 0 || count != buffers + handles || (count && !args))
        return -EINVAL;

    struct vendor_arg packed[MAX_ARGS];
    int32_t fds[MAX_ARGS];
    uint32_t attrs[MAX_ARGS];
    unsigned long request = VENDOR_REQUEST(1, struct vendor_invoke);
    int has_fds = 0, has_attrs = 0;
    for (size_t i = 0; i < count; i++) {
        const struct pico_fastrpc_arg *arg = &args[i];
        if (arg->fd < -1 || (arg->attributes & ~UINT32_C(127)))
            return -EINVAL;
        if (arg->pointer > UINTPTR_MAX || arg->length > SIZE_MAX)
            return -EOVERFLOW;
        if (i < buffers) {
            if (arg->length > PTRDIFF_MAX || arg->length > UINTPTR_MAX - arg->pointer)
                return -EOVERFLOW;
        } else if (arg->pointer > UINT32_MAX || arg->length > UINT32_MAX || arg->fd < 0) {
            return -EINVAL;
        }
        packed[i] = (struct vendor_arg){(uintptr_t)arg->pointer, (size_t)arg->length};
        fds[i] = arg->fd;
        attrs[i] = arg->attributes;
        has_fds |= arg->fd >= 0;
        has_attrs |= arg->attributes != 0;
    }
    struct vendor_invoke_crc call = {
        .invoke =
            {
                .invoke =
                    {
                        .invoke = {.handle = handle,
                                   .scalar = scalar,
                                   .args = count ? packed : NULL},
                        .fds = count ? fds : NULL,
                    },
                .attrs = count ? attrs : NULL,
            },
        .crc = crc,
    };
    if (has_fds)
        request = VENDOR_REQUEST(4, struct vendor_invoke_fd);
    if (has_attrs)
        request = VENDOR_REQUEST(7, struct vendor_invoke_attrs);
    if (crc)
        request = VENDOR_REQUEST(11, struct vendor_invoke_crc);
    int result = ioctl(dev, request, &call);
    return result < 0 ? -errno : result;
}
