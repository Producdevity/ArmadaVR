#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GFP_KERNEL 0
#define EPROBE_DEFER 517
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
struct device_node { int unused; };
enum of_gpio_flags { ACTIVE_LOW = 1 };
static struct device_node node;
static bool present, malformed, compatible, available, fail_alloc;
static int values[5], directions[5], gpios[5], flags[5], references;
static int requested[5], request_count, free_count, allocations, fail_request;
static unsigned checks;

static void *kzalloc(size_t size, int allocation_flags)
{
    assert(allocation_flags == GFP_KERNEL);
    if (fail_alloc)
        return NULL;
    ++allocations;
    return calloc(1, size);
}
static void kfree(void *p) { assert(allocations == 1); --allocations; free(p); }
static void *of_find_property(struct device_node *n, const char *name, void *length)
{
    assert(n == &node && !strcmp(name, "pico,native-mipi-mux") && !length);
    return present ? &node : NULL;
}
static struct device_node *of_parse_phandle(struct device_node *n, const char *name, int index)
{
    assert(n == &node && name && index == 0);
    if (malformed)
        return NULL;
    ++references;
    return &node;
}
static bool of_device_is_compatible(struct device_node *n, const char *name)
{
    assert(n == &node && !strcmp(name, "qcom,vxr7200")); return compatible;
}
static bool of_device_is_available(struct device_node *n) { assert(n == &node); return available; }
static void of_node_put(struct device_node *n) { assert(n == &node && references == 1); --references; }
static int of_get_named_gpio_flags(struct device_node *n, const char *name, int index,
                                  enum of_gpio_flags *result)
{
    static const char * const expected[] = {"qcom,vxr7200-rst", "qcom,vxr7200-3v3en",
        "qcom,vxr7200-1v0en", "qcom,vxr7200-1v8en", "qcom,vxr7200-mipi_switch"};
    assert(n == &node && !index);
    for (unsigned i = 0; i < ARRAY_SIZE(expected); ++i) {
        if (!strcmp(expected[i], name)) { *result = flags[i]; return gpios[i]; }
    }
    abort();
}
static bool gpio_is_valid(int gpio) { return gpio >= 0 && gpio < 5; }
static int gpio_request(int gpio, const char *name)
{
    assert(gpio_is_valid(gpio) && !requested[gpio] && name);
    if (gpio == fail_request)
        return -EBUSY;
    requested[gpio] = 1; ++request_count; return 0;
}
static void gpio_free(int gpio)
{
    assert(gpio_is_valid(gpio) && requested[gpio]);
    requested[gpio] = 0; ++free_count;
}
static int gpio_to_desc(int gpio) { assert(gpio_is_valid(gpio) && requested[gpio]); return gpio; }
static int gpiod_get_direction(int gpio) { return directions[gpio]; }
static int gpio_get_value_cansleep(int gpio) { assert(requested[gpio]); return values[gpio]; }

#include "driver.inc"

static void reset(void)
{
    assert(!allocations && !references && request_count == free_count);
    for (unsigned i = 0; i < 5; ++i) {
        assert(!requested[i]); values[i] = directions[i] = flags[i] = 0; gpios[i] = i;
    }
    present = compatible = available = true;
    malformed = fail_alloc = false; fail_request = -1;
}
static void rejected(int error)
{
    struct pico_panel_mux *mux = pico_panel_mux_get(&node);
    assert(IS_ERR(mux) && PTR_ERR(mux) == error);
    assert(!allocations && !references && request_count == free_count);
    ++checks;
}
int main(void)
{
    reset(); present = false;
    assert(!pico_panel_mux_get(&node)); assert(!pico_panel_mux_prepare(NULL)); pico_panel_mux_put(NULL);
    reset(); malformed = true; rejected(-EINVAL);
    reset(); compatible = false; rejected(-ENODEV);
    reset(); available = false; rejected(-ENODEV);
    reset(); fail_alloc = true; rejected(-ENOMEM);
    for (unsigned i = 0; i < 5; ++i) {
        reset(); gpios[i] = -EPROBE_DEFER; rejected(-EPROBE_DEFER);
        reset(); gpios[i] = 5; rejected(-EINVAL);
        reset(); flags[i] = 1; rejected(-EINVAL);
        reset(); fail_request = i; rejected(-EBUSY);
        reset(); directions[i] = 1; rejected(-EBUSY);
        reset(); directions[i] = -EIO; rejected(-EIO);
        reset(); values[i] = 1; rejected(-EBUSY);
        reset(); values[i] = -EIO; rejected(-EIO);
        for (unsigned j = 0; j < i; ++j) {
            reset(); gpios[i] = gpios[j]; rejected(-EINVAL);
        }
    }
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        reset(); struct pico_panel_mux *mux = pico_panel_mux_get(&node);
        assert(mux && !IS_ERR(mux));
        for (unsigned i = 0; i < 5; ++i) {
            assert(!pico_panel_mux_prepare(mux));
            values[i] = 1; assert(pico_panel_mux_prepare(mux) == -EBUSY); values[i] = 0;
            directions[i] = 1; assert(pico_panel_mux_prepare(mux) == -EBUSY); directions[i] = 0;
            ++checks;
        }
        pico_panel_mux_put(mux);
    }
    reset();
    printf("%u panel mux ownership and state checks passed\n", checks);
    return 0;
}
