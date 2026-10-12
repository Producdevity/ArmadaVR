#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "remote.h"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            printf("PICO_RPCMEM_RUNTIME_FAIL line=%d errno=%d %s\n", __LINE__, errno, #x);         \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int read_text(const char *path, char *buffer, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t count = read(fd, buffer, size - 1);
    close(fd);
    if (count < 0)
        return -1;
    buffer[count] = 0;
    return 0;
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char board[128], command[2048];
    if (read_text("/proc/device-tree/compatible", board, sizeof(board)) ||
        strcmp(board, "linux,dummy-virt") || read_text("/proc/cmdline", command, sizeof(command)) ||
        !strstr(command, "armada.fastrpc_transport_test=1")) {
        puts("PICO_RPCMEM_RUNTIME_REFUSED");
        return 2;
    }
    CHECK(setenv("PICO_ION_HEAP_ID", "25", 1) == 0);
    void *library = dlopen("/runtime/libsdsprpc.so.1.0.0", RTLD_NOW | RTLD_LOCAL);
    if (!library)
        fprintf(stderr, "dlopen: %s\n", dlerror());
    CHECK(library);
    int (*open_remote)(const char *, uint32_t *) = dlsym(library, "remote_handle_open");
    void *(*allocate)(int, uint32_t, int) = dlsym(library, "rpcmem_alloc");
    int (*get_fd)(void *) = dlsym(library, "rpcmem_to_fd");
    void (*release)(void *) = dlsym(library, "rpcmem_free");
    int (*lookup)(void *, int, int *, void **, int *, int *) = dlsym(library, "fdlist_fd_from_buf");
    CHECK(open_remote && allocate && get_fd && release && lookup);
    uint32_t handle = 0;
    CHECK(open_remote(ITRANSPORT_PREFIX "createstaticpd:sensorspd", &handle) == 0);
    CHECK(handle == 258);
    const size_t sizes[] = {1, 4096, 8193, 851968};
    unsigned cases = 0;
    for (unsigned flags = 0; flags < 2; flags++) {
        for (unsigned n = 0; n < 4; n++) {
            size_t size = sizes[n];
            unsigned char *buffer = allocate(-1, flags, (int)size);
            CHECK(buffer);
            int fd = get_fd(buffer);
            CHECK(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
            int nova = 99, attrs = 99, found = -1;
            void *base = NULL;
            CHECK(lookup(buffer, (int)size, &nova, &base, &attrs, &found) == 0);
            CHECK(found == fd && attrs == 0 && nova == 0);
            uint64_t sync = 2;
            CHECK(ioctl(fd, 0x40086200UL, &sync) == 0);
            for (size_t i = 0; i < size; i++)
                CHECK(buffer[i] == 0);
            memset(buffer, 0xa5, size);
            sync = 6;
            CHECK(ioctl(fd, 0x40086200UL, &sync) == 0);
            release(buffer);
            CHECK(get_fd(buffer) == -1);
            errno = 0;
            CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
            cases++;
        }
    }
    errno = 0;
    CHECK(!allocate(-1, 0x10000000U, 4096) && errno == EINVAL);
    puts("PICO_RPCMEM_RUNTIME_BUFFER_PASS");
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        handle = 0;
        int result = open_remote("sns_low_lat_stream", &handle);
        CHECK(result != 0);
        printf("PICO_RPCMEM_RUNTIME_NO_DSP attempt=%u status=%d\n", attempt, result);
    }
    CHECK(dlclose(library) == 0);
    printf("PICO_RPCMEM_RUNTIME_PASS cases=%u\n", cases);
    return 0;
}
