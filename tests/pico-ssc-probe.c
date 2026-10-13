#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

struct class { int unused; };
struct device { int unused; };
struct platform_device { int unused; };
struct file_operations { int unused; };
struct kobject { unsigned refs; bool dynamic; };
struct cdev { struct kobject kobj; const struct file_operations *ops; void *owner; };
struct sns_ssc_control_s { struct class *dev_class; dev_t dev_num; struct device *dev; struct cdev *cdev; };
static struct sns_ssc_control_s sns_ctl;
static const struct file_operations sensors_ssc_fops = {0};
#define CLASS_NAME "ssc"
#define DRV_NAME "sensors"
#define THIS_MODULE NULL
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define pr_err(...) ((void)0)
#define dev_err(...) ((void)0)
static unsigned stage, endpoint, class_live, region_live, device_live, cdev_live, premature_frees;
static unsigned after_class_error, frees, unmaps;
static struct class device_class;
static struct device device;
static struct cdev object;
static int failure;

static int slpi_loader_init_sysfs(struct platform_device *p)
{ (void)p; if (stage == 1) return failure; endpoint = 1; return 0; }
static int slpi_loader_remove(struct platform_device *p) { (void)p; endpoint = 0; return 0; }
static struct class *class_create(void *owner, const char *name)
{
    (void)owner; assert(!strcmp(name, CLASS_NAME));
    if (stage == 2) return (void *)(intptr_t)failure;
    class_live = 1; return &device_class;
}
static void class_destroy(struct class *c) { if (IS_ERR(c)) return; assert(c == &device_class && class_live); class_live = 0; }
static int alloc_chrdev_region(dev_t *dev, unsigned first, unsigned count, const char *name)
{
    assert(!first && count == 1 && !strcmp(name, DRV_NAME));
    if (stage == 3) return failure;
    *dev = 1; region_live = 1; return 0;
}
static void unregister_chrdev_region(dev_t dev, unsigned count)
{ assert(dev == 1 && count == 1 && region_live); region_live = 0; }
static struct device *device_create(struct class *c, void *parent, dev_t dev, void *data, const char *name)
{
    (void)parent; (void)data;
    if (IS_ERR(c)) { ++after_class_error; return (void *)(intptr_t)-EFAULT; }
    assert(c == &device_class && dev == 1 && !strcmp(name, DRV_NAME));
    if (stage == 4) return (void *)(intptr_t)failure;
    device_live = 1; return &device;
}
static void device_destroy(struct class *c, dev_t dev)
{ assert(c == &device_class && dev == 1 && device_live); device_live = 0; }
static struct cdev *cdev_alloc(void)
{
    if (stage == 5) return NULL;
    memset(&object, 0, sizeof(object)); object.kobj.refs = 1; object.kobj.dynamic = true;
    cdev_live = 1; return &object;
}
static void cdev_init(struct cdev *c, const struct file_operations *ops)
{ memset(c, 0, sizeof(*c)); c->kobj.refs = 1; c->ops = ops; }
static void kfree(struct cdev *c)
{
    assert(c == &object && cdev_live);
    premature_frees += c->kobj.refs > 0; cdev_live = 0; ++frees;
}
static void kobject_put(struct kobject *k)
{
    assert(k == &object.kobj && k->refs);
    if (--k->refs == 0 && k->dynamic) kfree(&object);
}
static int cdev_add(struct cdev *c, dev_t dev, unsigned count)
{ assert(c == &object && c->ops == &sensors_ssc_fops && dev == 1 && count == 1); return stage == 6 ? failure : 0; }
static void cdev_del(struct cdev *c) { ++unmaps; kobject_put(&c->kobj); }

#include "probe.inc"

int main(void)
{
    unsigned cases = 0, defects = 0;
    const int errors[] = {-EIO, -ENOMEM, -517};
    for (unsigned e = 0; e < sizeof(errors) / sizeof(*errors); ++e) {
        for (stage = 1; stage <= 6; ++stage) {
            memset(&sns_ctl, 0, sizeof(sns_ctl)); failure = errors[e];
            endpoint = class_live = region_live = device_live = cdev_live = 0;
            premature_frees = after_class_error = 0;
            struct platform_device p = {0};
            int rc = sensors_ssc_probe(&p);
            int expected = stage == 5 ? -ENOMEM : failure;
            defects += rc != expected || after_class_error || premature_frees;
            assert(!endpoint && !class_live && !region_live && !device_live && !cdev_live);
            ++cases;
        }
    }
    for (unsigned open_files = 0; open_files <= 128; ++open_files) {
        stage = 0; premature_frees = after_class_error = unmaps = frees = 0;
        struct platform_device p = {0}; assert(sensors_ssc_probe(&p) == 0);
        object.kobj.refs += open_files;
        assert(sensors_ssc_remove(&p) == 0);
        defects += premature_frees != 0 || cdev_live != (open_files != 0);
        assert(!endpoint && !class_live && !region_live && !device_live && !sns_ctl.cdev);
        assert(unmaps == 1);
#ifndef EXPECT_OLD
        for (unsigned i = 0; i < open_files; ++i) kobject_put(&object.kobj);
        assert(!cdev_live && frees == 1 && !premature_frees);
#endif
        ++cases;
    }
#ifdef EXPECT_OLD
    assert(defects > 128);
#else
    assert(defects == 0);
#endif
    printf("PICO_SSC_PROBE_PASS cases=%u defects=%u\n", cases, defects);
}
