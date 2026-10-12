#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "../src/pico-fastrpc.c"
#include "adsprpc_shared.h"

#define SAME_SIZE(a, b) _Static_assert(sizeof(struct a) == sizeof(struct b), #a " size")
#define SAME_OFFSET(a, x, b, y) \
    _Static_assert(offsetof(struct a, x) == offsetof(struct b, y), #a " " #x " offset")

SAME_SIZE(vendor_arg, remote_buf);
SAME_SIZE(vendor_invoke, fastrpc_ioctl_invoke);
SAME_SIZE(vendor_invoke_fd, fastrpc_ioctl_invoke_fd);
SAME_SIZE(vendor_invoke_attrs, fastrpc_ioctl_invoke_attrs);
SAME_SIZE(vendor_invoke_crc, fastrpc_ioctl_invoke_crc);
SAME_SIZE(vendor_init, fastrpc_ioctl_init);
SAME_OFFSET(vendor_arg, pointer, remote_buf, pv);
SAME_OFFSET(vendor_arg, length, remote_buf, len);
SAME_OFFSET(vendor_invoke, handle, fastrpc_ioctl_invoke, handle);
SAME_OFFSET(vendor_invoke, scalar, fastrpc_ioctl_invoke, sc);
SAME_OFFSET(vendor_invoke, args, fastrpc_ioctl_invoke, pra);
SAME_OFFSET(vendor_invoke_fd, fds, fastrpc_ioctl_invoke_fd, fds);
SAME_OFFSET(vendor_invoke_attrs, attrs, fastrpc_ioctl_invoke_attrs, attrs);
SAME_OFFSET(vendor_invoke_crc, crc, fastrpc_ioctl_invoke_crc, crc);
SAME_OFFSET(vendor_init, flags, fastrpc_ioctl_init, flags);
SAME_OFFSET(vendor_init, file, fastrpc_ioctl_init, file);
SAME_OFFSET(vendor_init, file_length, fastrpc_ioctl_init, filelen);
SAME_OFFSET(vendor_init, file_fd, fastrpc_ioctl_init, filefd);
SAME_OFFSET(vendor_init, memory, fastrpc_ioctl_init, mem);
SAME_OFFSET(vendor_init, memory_length, fastrpc_ioctl_init, memlen);
SAME_OFFSET(vendor_init, memory_fd, fastrpc_ioctl_init, memfd);

_Static_assert(VENDOR_REQUEST(1, struct vendor_invoke) == FASTRPC_IOCTL_INVOKE, "INVOKE");
_Static_assert(VENDOR_REQUEST(4, struct vendor_invoke_fd) == FASTRPC_IOCTL_INVOKE_FD, "INVOKE_FD");
_Static_assert(VENDOR_REQUEST(7, struct vendor_invoke_attrs) == FASTRPC_IOCTL_INVOKE_ATTRS, "ATTRS");
_Static_assert(VENDOR_REQUEST(11, struct vendor_invoke_crc) == FASTRPC_IOCTL_INVOKE_CRC, "CRC");
_Static_assert(VENDOR_REQUEST(6, struct vendor_init) == FASTRPC_IOCTL_INIT, "INIT");
_Static_assert(VENDOR_REQUEST(8, uint32_t) == FASTRPC_IOCTL_GETINFO, "GETINFO");
_Static_assert(PICO_FASTRPC_NOVA == FASTRPC_ATTR_NOVA, "NOVA");
_Static_assert(PICO_FASTRPC_NON_COHERENT == FASTRPC_ATTR_NON_COHERENT, "NON_COHERENT");
_Static_assert(PICO_FASTRPC_COHERENT == FASTRPC_ATTR_COHERENT, "COHERENT");
_Static_assert(PICO_FASTRPC_KEEP_MAP == FASTRPC_ATTR_KEEP_MAP, "KEEP_MAP");
_Static_assert(PICO_FASTRPC_NOMAP == FASTRPC_ATTR_NOMAP, "NOMAP");
_Static_assert(PICO_FASTRPC_NOFLUSH == FASTRPC_ATTR_FORCE_NOFLUSH, "NOFLUSH");
_Static_assert(PICO_FASTRPC_NOINVALIDATE == FASTRPC_ATTR_FORCE_NOINVALIDATE, "NOINVALIDATE");

int main(void) { return 0; }
