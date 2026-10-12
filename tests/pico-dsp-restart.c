#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p) >= (uintptr_t)-4095)
#define SSR_RESET_CMD 1
#define dev_dbg(dev, ...) ((void)(dev))
#define dev_err(dev, ...) ((void)(dev))
#define pr_debug(...) ((void)0)

struct kobject { int unused; };
struct kobj_attribute { int unused; };
struct subsys_device { int unused; };
struct platform_device { int dev; void *data; };
struct adsp_loader_private { void *pil_h; };
struct slpi_loader_private { void *pil_h; };
static struct platform_device *adsp_private, *slpi_private;
static unsigned int calls, invalid_calls;
static int restart_result;

static void *platform_get_drvdata(struct platform_device *pdev)
{
    return pdev->data;
}

static int kstrtoint(const char *s, unsigned int base, int *result)
{
    char *end;
    long value = strtol(s, &end, base);
    if (end == s || (*end && *end != '\n') || value < INT32_MIN || value > INT32_MAX)
        return -EINVAL;
    *result = (int)value;
    return 0;
}

static int subsystem_restart_dev(struct subsys_device *dev)
{
    ++calls;
    if (IS_ERR_OR_NULL(dev)) {
        ++invalid_calls;
        return -EFAULT;
    }
    return restart_result;
}

#include "audio-kernel/dsp/adsp-loader.c"
#include "drivers/sensors/sensors_ssc.c"

static void require(int condition)
{
    if (!condition)
        abort();
}

int main(void)
{
    struct adsp_loader_private adsp = {0};
    struct slpi_loader_private slpi = {0};
    struct platform_device a = {.data = &adsp}, s = {.data = &slpi};
    struct subsys_device valid = {0};
    adsp_private = &a;
    slpi_private = &s;
    for (int error = 1; error <= 4095; ++error) {
        adsp.pil_h = slpi.pil_h = (void *)(intptr_t)-error;
        unsigned int before = calls;
        ssize_t ar = adsp_ssr_store(NULL, NULL, "1\n", 2);
        ssize_t sr = slpi_ssr_store(NULL, NULL, "1\n", 2);
#ifdef EXPECT_OLD
        require(calls == before + 2 && ar == -EFAULT && sr == -EINVAL);
#else
        require(calls == before && ar == -EINVAL && sr == -EINVAL);
#endif
    }
    adsp.pil_h = slpi.pil_h = NULL;
    unsigned int before = calls;
    require(adsp_ssr_store(NULL, NULL, "1", 1) == -EINVAL);
    require(slpi_ssr_store(NULL, NULL, "1", 1) == -EINVAL);
    require(calls == before);
    adsp.pil_h = slpi.pil_h = &valid;
    const char *invalid[] = {"bad", "0", "2", "-1", "2147483648"};
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        require(adsp_ssr_store(NULL, NULL, invalid[i], 1) == -EINVAL);
        require(slpi_ssr_store(NULL, NULL, invalid[i], 1) == -EINVAL);
        require(calls == before);
    }
    for (int rc = -3; rc <= 0; ++rc) {
        restart_result = rc;
        require(adsp_ssr_store(NULL, NULL, "1", 1) == (rc ? rc : 1));
        require(slpi_ssr_store(NULL, NULL, "1", 1) == (rc ? -EINVAL : 1));
        require(calls == before + 2);
        before = calls;
    }
#ifdef EXPECT_OLD
    require(invalid_calls == 8190);
#else
    require(invalid_calls == 0);
#endif
    printf("8190 failed acquisitions; %u invalid restart dispatches\n", invalid_calls);
    return 0;
}
