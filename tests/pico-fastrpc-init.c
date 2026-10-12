#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_CHANNELS 4
#define ADSP_DOMAIN_ID 0
#define FASTRPC_MODE_UNSIGNED_MODULE 8
#define MINOR_NUM_DEV 0

struct fastrpc_channel_ctx { int unsigned_support, secure; };
struct fastrpc_apps { struct fastrpc_channel_ctx channel[NUM_CHANNELS]; };
struct fastrpc_file { int cid, dev_minor; };
struct fastrpc_ioctl_init { int flags; };
struct fastrpc_ioctl_init_attrs { struct fastrpc_ioctl_init init; int attrs; };
struct fastrpc_ioctl_invoke_crc { int unused; };
struct smq_phy_page { int unused; };
struct fastrpc_mmap { int unused; };
struct fastrpc_buf { int unused; };
static struct fastrpc_apps *test_apps;
#define gfa (*test_apps)
static unsigned int opens;
static int transport_result;

static int fastrpc_channel_open(struct fastrpc_file *fl)
{
    assert(fl->cid >= 0 && fl->cid < NUM_CHANNELS);
    ++opens;
    return transport_result;
}

#include "drivers/char/adsprpc.c"

int main(int argc, char **argv)
{
    test_apps = calloc(1, sizeof(*test_apps));
    assert(test_apps);
    struct fastrpc_file file = {0};
    struct fastrpc_ioctl_init_attrs init = {0};
    if (argc == 2) {
        file.cid = atoi(argv[1]);
        return fastrpc_init_process(&file, &init);
    }
    unsigned int cases = 0;
    for (int cid = 0; cid < NUM_CHANNELS; ++cid)
        for (int support = 0; support < 2; ++support)
            for (int secure = 0; secure < 2; ++secure)
                for (int minor = 0; minor < 2; ++minor)
                    for (int unsigned_request = 0; unsigned_request < 2; ++unsigned_request)
                        for (int error = -3; error <= 0; ++error) {
                            file.cid = cid;
                            file.dev_minor = minor;
                            init.attrs = unsigned_request ? FASTRPC_MODE_UNSIGNED_MODULE : 0;
                            gfa.channel[cid].unsigned_support = support;
                            gfa.channel[cid].secure = secure;
                            transport_result = error;
                            unsigned int before = opens;
                            int denied = support && secure && minor == MINOR_NUM_DEV && !unsigned_request;
                            int result = fastrpc_init_process(&file, &init);
                            assert(result == (denied ? -ECONNREFUSED : error));
                            assert(opens == before + !denied);
                            ++cases;
                        }
#ifndef EXPECT_OLD
    int invalid[] = {INT_MIN, -65536, -2, -1, NUM_CHANNELS, NUM_CHANNELS + 1, 65536, INT_MAX};
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        file.cid = invalid[i];
        unsigned int before = opens;
        assert(fastrpc_init_process(&file, &init) == -EINVAL);
        assert(opens == before);
    }
#endif
    free(test_apps);
    printf("%u valid policy and transport cases passed\n", cases);
    return 0;
}
