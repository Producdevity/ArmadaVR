#define _GNU_SOURCE
#ifdef NDEBUG
#error "Assertions must be enabled"
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int mock_open(const char *path, int flags, ...);
static int mock_close(int fd);
#define open mock_open
#define close mock_close
#include "../src/pico-rpcmem.c"
#undef open
#undef close

static unsigned opens, closes, allocated, released, registered, unregistered;
static unsigned expected_heap;
static size_t expected_size;
static int expected_cached, allocation_error, release_error, register_error, unregister_error;
static void *last_buffer;
static int next_fd;

static int mock_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, "/dev/ion") && flags == (O_RDONLY | O_CLOEXEC));
    opens++;
    return 7;
}
static int mock_close(int fd)
{
    assert(fd == 7);
    closes++;
    return 0;
}

int ion_buffer_allocate(int fd, uint32_t heap, size_t size, bool cached, struct ion_buffer *buffer)
{
    assert(fd == 7 && heap == expected_heap && size == expected_size && cached == expected_cached);
    assert(buffer->fd == -1 && !buffer->data && !buffer->size);
    if (allocation_error)
        return allocation_error;
    size_t rounded = (size + 4095) & ~(size_t)4095;
    last_buffer = malloc(rounded);
    assert(last_buffer);
    *buffer = (struct ion_buffer){next_fd++, last_buffer, rounded};
    allocated++;
    return 0;
}
int ion_buffer_release(struct ion_buffer *buffer)
{
    assert(buffer->fd >= 0 && buffer->data && buffer->size);
    if (release_error && release_error != -EINTR)
        return release_error;
    free(buffer->data);
    *buffer = (struct ion_buffer){.fd = -1};
    released++;
    return release_error;
}
int pico_rpcmem_register(void *buffer, size_t size, int fd)
{
    assert(buffer == last_buffer && size == expected_size);
    if (fd >= 0) {
        if (register_error)
            return register_error;
        registered++;
    } else {
        assert(fd == -1);
        if (unregister_error)
            return unregister_error;
        unregistered++;
    }
    return 0;
}

int main(void)
{
    assert(setenv("PICO_ION_HEAP_ID", "25", 1) == 0);
    rpcmem_init();
    rpcmem_init();
    assert(opens == 1);
    expected_heap = 25;
    const uint32_t flags[] = {1, RPCMEM_HEAP_DEFAULT, RPCMEM_HEAP_DEFAULT | RPCMEM_HEAP_UNCACHED, 0,
                              1 | RPCMEM_HEAP_NOREG};
    const size_t sizes[] = {1, 4096, 8193, 851968};
    for (unsigned f = 0; f < 5; f++) {
        expected_cached = f == 0 || f == 1 || f == 4;
        for (unsigned n = 0; n < 4; n++) {
            expected_size = sizes[n];
            void *p = rpcmem_alloc(-1, flags[f], (int)expected_size);
            assert(p && rpcmem_to_fd(p) == (int)allocated - 1);
            assert(rpcmem_to_fd((char *)p + 1) == -1);
            rpcmem_free(p);
            assert(!allocations);
            rpcmem_free(p);
        }
    }
    assert(allocated == 20 && released == 20 && registered == 16 && unregistered == 16);
    expected_size = 4097;
    expected_cached = 1;
    allocation_error = -ENOMEM;
    assert(!rpcmem_alloc(-1, 1, 4097) && errno == ENOMEM && !allocations);
    allocation_error = 0;
    register_error = 1;
    assert(!rpcmem_alloc(-1, 1, 4097) && errno == EIO && !allocations);
    assert(allocated == released);
    register_error = 0;
    void *p = rpcmem_alloc(-1, 1, 4097);
    assert(p);
    int fd = rpcmem_to_fd(p);
    unregister_error = 1;
    rpcmem_free(p);
    assert(rpcmem_to_fd(p) == fd && allocations->registered);
    unregister_error = 0;
    release_error = -EIO;
    rpcmem_free(p);
    assert(rpcmem_to_fd(p) == fd && !allocations->registered);
    release_error = -EINTR;
    rpcmem_free(p);
    assert(!allocations);
    release_error = 0;
    assert(allocated == released && registered == unregistered);
    assert(!rpcmem_alloc(-1, 1, 0));
    assert(!rpcmem_alloc(-1, 1, -1));
    assert(!rpcmem_alloc2(-1, 1, SIZE_MAX));
    assert(!rpcmem_alloc(-1, RPCMEM_HEAP_NOVA, 4097));
    assert(!rpcmem_alloc(32, 1, 4097));
    assert(!rpcmem_alloc(-2, 1, 4097));
    rpcmem_free(NULL);
    assert(allocated == released);
    p = rpcmem_alloc(-1, 1, 4097);
    assert(p);
    rpcmem_deinit();
    assert(rpcmem_to_fd(p) >= 0);
    rpcmem_free(p);
    rpcmem_deinit();
    assert(opens == 1 && closes == 1 && !allocations);
    const char *bad[] = {"", "-1", "32", "25x", " 25", "999999999999999999999999"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        assert(setenv("PICO_ION_HEAP_ID", bad[i], 1) == 0);
        rpcmem_init();
        assert(!rpcmem_alloc(-1, 1, 4097) && errno == ENODEV);
        rpcmem_deinit();
    }
    assert(allocated == released && registered == unregistered && opens == closes);
    puts("PICO_RPCMEM_PASS allocation_modes=20 ownership_faults=5 heap_refusals=6");
    return 0;
}
