#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

struct class { int unused; };
struct device { void *of_node; };
struct cdev { int unused; };
struct platform_device { struct device dev; void *data; };
struct kobject { int unused; };
struct attribute { int unused; };
struct kobj_attribute { struct attribute attr; };
struct attribute_group { struct attribute **attrs; };
struct subsys_device { int unused; };
struct work_struct { void (*fn)(struct work_struct *); atomic_bool pending; };
#define DEFINE_MUTEX(n) pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER
#define DECLARE_WORK(n, f) struct work_struct n = {.fn = f}
#define INIT_WORK(w, f) ((w)->fn = (f))
#define mutex_lock(m) assert(!pthread_mutex_lock(m))
#define mutex_unlock(m) assert(!pthread_mutex_unlock(m))
#define __ATTR(...) {.attr = {0}}
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define GFP_KERNEL 0
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define pr_debug(...) ((void)0)
#define pr_err(...) ((void)0)
static struct kobject *kernel_kobj;
static void *allocations[16];
static unsigned allocation_count, kobjects, removes, get_calls, put_calls, restarts, unpublished_work;
static int allocation_failure, group_error, property_error, get_error, restart_error;
static atomic_uint references;
static struct subsys_device handle;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool block_get, get_entered, release_get;
static atomic_bool work_running, stop_entered, stop_finished;

static void *devm_kzalloc(struct device *d, size_t size, int flags)
{
    (void)d; (void)flags;
    if (allocation_failure && (int)allocation_count + 1 == allocation_failure) return NULL;
    assert(allocation_count < 16);
    return allocations[allocation_count++] = calloc(1, size);
}
static void *platform_get_drvdata(struct platform_device *p) { assert(p); return p->data; }
static void platform_set_drvdata(struct platform_device *p, void *data) { p->data = data; }
static int of_property_read_string(void *node, const char *key, const char **value)
{
    assert(node && !strcmp(key, "qcom,firmware-name")); *value = "slpi"; return property_error;
}
static int kstrtoint(const char *s, unsigned base, int *result)
{
    char *end; errno = 0; long n = strtol(s, &end, base);
    if (end == s || (*end && strcmp(end, "\n")) || errno || n < INT_MIN || n > INT_MAX) return -EINVAL;
    *result = (int)n; return 0;
}
static void *subsystem_get_with_fwname(const char *name, const char *firmware)
{
    assert(!strcmp(name, "slpi") && !strcmp(firmware, "slpi"));
    mutex_lock(&gate);
    ++get_calls; get_entered = true; assert(!pthread_cond_broadcast(&changed));
    while (block_get && !release_get) assert(!pthread_cond_wait(&changed, &gate));
    mutex_unlock(&gate);
    if (get_error) return (void *)(intptr_t)get_error;
    atomic_fetch_add(&references, 1); return &handle;
}
static void subsystem_put(void *h)
{
    if (IS_ERR_OR_NULL(h)) return;
    assert(h == &handle && atomic_load(&references));
    atomic_fetch_sub(&references, 1); ++put_calls;
}
static int subsystem_restart_dev(struct subsys_device *h)
{ assert(h == &handle && atomic_load(&references)); ++restarts; return restart_error; }
static bool schedule_work(struct work_struct *w)
{ assert(w->fn); return !atomic_exchange(&w->pending, true); }
static bool cancel_work_sync(struct work_struct *w)
{
    bool pending = atomic_exchange(&w->pending, false);
    mutex_lock(&gate);
    while (atomic_load(&work_running)) assert(!pthread_cond_wait(&changed, &gate));
    mutex_unlock(&gate); return pending;
}
static struct kobject *kobject_create_and_add(const char *name, struct kobject *parent)
{ (void)parent; assert(!strcmp(name, "boot_slpi")); ++kobjects; return calloc(1, sizeof(struct kobject)); }
static void kobject_put(struct kobject *obj) { assert(kobjects); --kobjects; free(obj); }
static void kobject_del(struct kobject *obj) { kobject_put(obj); }
static int sysfs_create_group(struct kobject *obj, struct attribute_group *group);
static void sysfs_remove_group(struct kobject *obj, struct attribute_group *group)
{ assert(obj && group); ++removes; }

#include "driver.inc"

static int sysfs_create_group(struct kobject *obj, struct attribute_group *group)
{
    assert(obj && group); unpublished_work += !slpi_ldr_work.fn; return group_error;
}
static void run_work(void)
{
    if (atomic_exchange(&slpi_ldr_work.pending, false)) slpi_ldr_work.fn(&slpi_ldr_work);
}
static void *worker(void *unused)
{
    (void)unused; atomic_store(&work_running, true); run_work();
    mutex_lock(&gate); atomic_store(&work_running, false);
    assert(!pthread_cond_broadcast(&changed)); mutex_unlock(&gate); return NULL;
}
static void *stopper(void *remove_device)
{
    atomic_store(&stop_entered, true);
    if (remove_device) slpi_loader_remove(remove_device);
    else assert(slpi_boot_store(NULL, NULL, "0\n", 2) == 2);
    atomic_store(&stop_finished, true); return NULL;
}
static void reset(void)
{
    assert(!atomic_load(&work_running));
    slpi_private = NULL; atomic_store(&slpi_ldr_work.pending, false);
#ifdef EXPECT_OLD
    slpi_ldr_work.fn = NULL;
#endif
    allocation_count = kobjects = removes = get_calls = put_calls = restarts = 0;
    allocation_failure = group_error = property_error = get_error = restart_error = 0;
    atomic_store(&references, 0); block_get = get_entered = release_get = false;
    atomic_store(&stop_entered, false); atomic_store(&stop_finished, false);
}
static void init(struct platform_device *p)
{
    p->dev.of_node = p; p->data = NULL;
    assert(slpi_loader_init_sysfs(p) == 0);
#ifdef EXPECT_OLD
    INIT_WORK(&slpi_ldr_work, slpi_load_fw);
#endif
}
static void dispose(struct platform_device *p)
{
    slpi_loader_remove(p);
    slpi_private = NULL; atomic_store(&slpi_ldr_work.pending, false);
    for (unsigned i = 0; i < allocation_count; ++i) free(allocations[i]);
    assert(!kobjects);
}
int main(void)
{
    unsigned cases = 0, defects = 0;
    for (unsigned cycle = 0; cycle < 128; ++cycle) {
        struct platform_device p = {0};
        reset(); init(&p);
        assert(slpi_boot_store(NULL, NULL, "1\n", 2) == 2); run_work();
        assert(slpi_boot_store(NULL, NULL, "1\n", 2) == 2); run_work();
        assert(slpi_boot_store(NULL, NULL, "0\n", 2) == 2);
        defects += get_calls != 1 || put_calls != 1 || atomic_load(&references) != 0;
        dispose(&p); ++cases;

        reset(); init(&p);
        assert(slpi_boot_store(NULL, NULL, "1", 1) == 1);
        assert(slpi_boot_store(NULL, NULL, "0", 1) == 1); run_work();
        defects += get_calls != 0 || atomic_load(&references) != 0;
        dispose(&p); ++cases;

        reset(); init(&p);
        assert(slpi_boot_store(NULL, NULL, "1", 1) == 1);
        slpi_loader_remove(&p);
        defects += atomic_load(&slpi_ldr_work.pending) || slpi_private != NULL;
        dispose(&p); ++cases;

        for (unsigned removal = 0; removal < 2; ++removal) {
            reset(); init(&p); block_get = true;
            assert(slpi_boot_store(NULL, NULL, "1", 1) == 1);
            pthread_t w, stop; assert(!pthread_create(&w, NULL, worker, NULL));
            mutex_lock(&gate);
            while (!get_entered) assert(!pthread_cond_wait(&changed, &gate));
            mutex_unlock(&gate);
            assert(!pthread_create(&stop, NULL, stopper, removal ? &p : NULL));
#ifdef EXPECT_OLD
            assert(!pthread_join(stop, NULL));
#else
            while (!atomic_load(&stop_entered)) { }
#endif
            mutex_lock(&gate); release_get = true; assert(!pthread_cond_broadcast(&changed)); mutex_unlock(&gate);
            assert(!pthread_join(w, NULL));
#ifndef EXPECT_OLD
            assert(!pthread_join(stop, NULL));
#endif
            assert(atomic_load(&stop_finished));
            defects += atomic_load(&references) != 0;
            dispose(&p); ++cases;
        }
    }
#ifndef EXPECT_OLD
    const char *bad[] = {"", "garbage", "1junk", "0junk", "2", "-1", "2147483648", "1\n2"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        struct platform_device p = {0}; reset(); init(&p);
        assert(slpi_boot_store(NULL, NULL, bad[i], strlen(bad[i])) == -EINVAL);
        assert(!atomic_load(&slpi_ldr_work.pending)); dispose(&p); ++cases;
    }
    for (int error = 1; error <= 4095; ++error) {
        struct platform_device p = {0}; reset(); init(&p); get_error = -error;
        assert(slpi_boot_store(NULL, NULL, "1", 1) == 1); run_work();
        assert(!((struct slpi_loader_private *)p.data)->pil_h);
        assert(slpi_ssr_store(NULL, NULL, "1", 1) == -EINVAL);
        get_error = 0; assert(slpi_boot_store(NULL, NULL, "1", 1) == 1); run_work();
        assert(get_calls == 2 && atomic_load(&references) == 1);
        assert(slpi_ssr_store(NULL, NULL, "1", 1) == 1);
        restart_error = -EIO; assert(slpi_ssr_store(NULL, NULL, "1", 1) == -EINVAL);
        dispose(&p); assert(!atomic_load(&references));
        assert(slpi_boot_store(NULL, NULL, "1", 1) == -ENODEV);
        assert(slpi_ssr_store(NULL, NULL, "1", 1) == -ENODEV); ++cases;
    }
    for (int error = 1; error <= 3; ++error) {
        struct platform_device p = {.dev = {.of_node = &p}}; reset();
        if (error < 3) allocation_failure = error; else group_error = -EIO;
        assert(slpi_loader_init_sysfs(&p) == (error < 3 ? -ENOMEM : -EIO));
        assert(!slpi_private && !kobjects);
        for (unsigned i = 0; i < allocation_count; ++i) free(allocations[i]);
        ++cases;
    }
    struct platform_device p = {0}, other = {0}; reset(); init(&p);
    assert(slpi_loader_init_sysfs(&other) == -EBUSY && slpi_private == &p);
    property_error = -EINVAL; assert(slpi_boot_store(NULL, NULL, "1", 1) == 1); run_work();
    assert(!get_calls); property_error = 0;
    assert(slpi_boot_store(NULL, NULL, "1", 1) == 1); run_work();
    assert(get_calls == 1); dispose(&p); ++cases;
    assert(defects == 0 && unpublished_work == 0);
#else
    assert(defects == 640 && unpublished_work == 640);
#endif
    printf("PICO_DSP_LIFECYCLE_PASS cases=%u defects=%u premature_publications=%u\n", cases, defects, unpublished_work);
}
