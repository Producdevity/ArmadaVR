#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GFP_KERNEL 0
#define THIS_MODULE NULL
#define IRQ_HANDLED 1
#define IRQF_TRIGGER_RISING 1
#define IRQF_ONESHOT 0x2000
#define EPOLLIN 1
#define EPOLLRDNORM 0x40
#define EPOLLERR 8
#define EPOLLHUP 0x10
#define EPROBE_DEFER 517
#define __init
#define __exit
#define MODULE_DEVICE_TABLE(a, b)
#define MODULE_LICENSE(a)
#define MODULE_DESCRIPTION(a)
#define module_init(a)
#define module_exit(a)
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define mutex_lock(p) assert(!pthread_mutex_lock(p))
#define mutex_unlock(p) assert(!pthread_mutex_unlock(p))
#define spin_lock_init(p) assert(!pthread_mutex_init(p, NULL))
#define spin_lock(p) mutex_lock(p)
#define spin_unlock(p) mutex_unlock(p)
#define MKDEV(major, minor) ((unsigned)(major) << 20 | (unsigned)(minor))
#define READ_ONCE(v) __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
#define WRITE_ONCE(v, n) __atomic_store_n(&(v), (n), __ATOMIC_RELEASE)
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define no_llseek NULL

typedef pthread_mutex_t spinlock_t;
typedef struct { atomic_int value; } atomic_t;
#define atomic_read(p) atomic_load(&(p)->value)
#define atomic_set(p, n) atomic_store(&(p)->value, n)
#define atomic_inc(p) ((void)atomic_fetch_add(&(p)->value, 1))
typedef unsigned __poll_t;
typedef int irqreturn_t;
typedef struct { atomic_int wakes; } wait_queue_head_t;
typedef struct { wait_queue_head_t *queue; } poll_table;
struct kref { atomic_int references; };
struct inode { unsigned i_rdev; };
struct file { void *private_data; };
struct device { void *of_node; };
struct platform_device { struct device dev; void *data; };
struct class { int unused; };
struct of_device_id { const char *compatible; };
struct file_operations {
    void *owner;
    int (*open)(struct inode *, struct file *);
    __poll_t (*poll)(struct file *, poll_table *);
    int (*release)(struct inode *, struct file *);
    void *llseek;
};
struct platform_driver {
    int (*probe)(struct platform_device *);
    int (*remove)(struct platform_device *);
    struct { const char *name; const struct of_device_id *of_match_table; } driver;
};
static struct class class_storage;
static struct device device_storage;
static atomic_int allocations;
static atomic_int frees;
static int fail_alloc, gpio_result, irq_result, request_error, node_error;
static int chrdev_error, class_error, registration_error;
static unsigned nodes, irq_count, chrdev_count, class_count, registered;
static pthread_mutex_t irq_lock = PTHREAD_MUTEX_INITIALIZER;
static irqreturn_t (*irq_handler)(int, void *);
static void *irq_cookie;
static void *kzalloc(size_t size, int flags) {
    if (fail_alloc) { fail_alloc = 0; return NULL; }
    void *p = calloc(1, size); assert(p); atomic_fetch_add(&allocations, 1); return p;
}
static void kfree(void *p) { assert(p); atomic_fetch_add(&frees, 1); free(p); }
static void kref_init(struct kref *p) { atomic_init(&p->references, 1); }
static void kref_get(struct kref *p) { assert(atomic_fetch_add(&p->references, 1) > 0); }
static void kref_put(struct kref *p, void (*destroy)(struct kref *)) {
    int before = atomic_fetch_sub(&p->references, 1); assert(before > 0);
    if (before == 1) destroy(p);
}
static void init_waitqueue_head(wait_queue_head_t *p) { atomic_init(&p->wakes, 0); }
static void wake_up_interruptible(wait_queue_head_t *p) { atomic_fetch_add(&p->wakes, 1); }
static void poll_wait(struct file *f, wait_queue_head_t *p, poll_table *table) {
    if (table) table->queue = p;
}
static int nonseekable_open(struct inode *i, struct file *f) { return 0; }
static int of_get_named_gpio(void *node, const char *name, unsigned index) {
    assert(node && !strcmp(name, "station-sync") && index == 0); return gpio_result;
}
static int gpio_to_irq(int gpio) { assert(gpio == gpio_result); return irq_result; }
static const char *dev_name(struct device *d) { return "station"; }
static int request_threaded_irq(int irq, void *primary, irqreturn_t (*handler)(int, void *),
                                unsigned flags, const char *name, void *cookie) {
    assert(irq == irq_result && primary == NULL && flags == 0x2001 && name && cookie);
    if (request_error) return request_error;
    mutex_lock(&irq_lock);
    assert(!irq_cookie); irq_cookie = cookie; irq_handler = handler; ++irq_count;
    mutex_unlock(&irq_lock); return 0;
}
static void free_irq(int irq, void *cookie) {
    mutex_lock(&irq_lock);
    assert(irq == irq_result && cookie == irq_cookie && irq_count == 1);
    irq_handler = NULL; irq_cookie = NULL; --irq_count;
    mutex_unlock(&irq_lock);
}
static void fire_irq(void) {
    mutex_lock(&irq_lock);
    if (irq_handler) assert(irq_handler(irq_result, irq_cookie) == IRQ_HANDLED);
    mutex_unlock(&irq_lock);
}
static struct device *device_create(struct class *class, struct device *parent,
                                    unsigned dev, void *data, const char *name) {
    assert(class == &class_storage && parent && dev == MKDEV(42, 0));
    assert(!strcmp(name, "stationdev"));
    if (node_error) return (void *)(intptr_t)node_error;
    assert(!nodes); ++nodes; return &device_storage;
}
static void device_destroy(struct class *class, unsigned dev) {
    assert(class == &class_storage && nodes == 1); --nodes;
}
static void platform_set_drvdata(struct platform_device *p, void *data) { p->data = data; }
static void *platform_get_drvdata(struct platform_device *p) { return p->data; }
static int register_chrdev(int major, const char *name, const struct file_operations *ops) {
    assert(major == 0 && !strcmp(name, "stationdev") && ops->poll && ops->open && ops->release);
    if (chrdev_error) return chrdev_error;
    ++chrdev_count; return 42;
}
static void unregister_chrdev(int major, const char *name) { assert(chrdev_count == 1); --chrdev_count; }
static struct class *class_create(void *module, const char *name) {
    assert(!strcmp(name, "stationdev"));
    if (class_error) return (void *)(intptr_t)class_error;
    ++class_count; return &class_storage;
}
static void class_destroy(struct class *class) { assert(class_count == 1); --class_count; }
static int platform_driver_register(struct platform_driver *driver) {
    assert(!strcmp(driver->driver.of_match_table[0].compatible, "bytedance,station"));
    if (registration_error) return registration_error;
    ++registered; return 0;
}
static void platform_driver_unregister(struct platform_driver *driver) { assert(registered == 1); --registered; }

#include "driver.inc"

static unsigned cases;
static struct platform_device device(void) {
    struct platform_device result = { .dev.of_node = &device_storage };
    return result;
}
static void clean(void) {
    assert(!station_device && !nodes && !irq_count);
    assert(atomic_load(&allocations) == atomic_load(&frees));
    ++cases;
}
static void failure_cases(void) {
    struct platform_device p = device();
    p.dev.of_node = NULL; assert(station_probe(&p) == -ENODEV); clean();
    p = device(); gpio_result = -EPROBE_DEFER;
    assert(station_probe(&p) == -EPROBE_DEFER); clean(); gpio_result = 10;
    fail_alloc = 1; assert(station_probe(&p) == -ENOMEM); clean();
    irq_result = -EPROBE_DEFER; assert(station_probe(&p) == -EPROBE_DEFER); clean(); irq_result = 30;
    request_error = -EBUSY; assert(station_probe(&p) == -EBUSY); clean(); request_error = 0;
    node_error = -ENOMEM; assert(station_probe(&p) == -ENOMEM); clean(); node_error = 0;
}
static void reader_cases(void) {
    struct platform_device p = device(), second = device();
    struct inode inode = { MKDEV(42, 0) }, missing = { MKDEV(42, 1) };
    struct file a = {0}, b = {0}; poll_table table = {0};
    assert(station_open(&inode, &a) == -ENODEV);
    assert(!station_probe(&p));
    assert(station_probe(&second) == -EBUSY);
    assert(station_open(&missing, &a) == -ENODEV);
    fail_alloc = 1; assert(station_open(&inode, &a) == -ENOMEM);
    assert(!station_open(&inode, &a));
    assert(!station_poll(&a, &table) && table.queue == &station_device->wait);
    fire_irq();
    assert(!station_open(&inode, &b));
    assert(station_poll(&a, NULL) == (EPOLLIN | EPOLLRDNORM));
    assert(!station_poll(&a, NULL) && !station_poll(&b, NULL));
    fire_irq(); fire_irq();
    assert(station_poll(&a, NULL) == (EPOLLIN | EPOLLRDNORM));
    assert(station_poll(&b, NULL) == (EPOLLIN | EPOLLRDNORM));
    assert(!station_poll(&a, NULL));
    atomic_set(&station_device->sequence, INT32_MAX);
    assert(station_poll(&a, NULL)); fire_irq(); assert(station_poll(&a, NULL));
    struct pico_station *old = station_device;
    int wakes = atomic_load(&old->wait.wakes);
    assert(!station_remove(&p));
    assert(atomic_load(&old->wait.wakes) == wakes + 1);
    assert(station_poll(&a, NULL) == (EPOLLERR | EPOLLHUP));
    assert(!station_probe(&second));
    assert(station_poll(&b, NULL) == (EPOLLERR | EPOLLHUP));
    assert(!station_release(&inode, &a) && !station_release(&inode, &b));
    assert(!station_remove(&second)); clean();
}
struct race { struct file *file; atomic_bool go; };
static void *irq_worker(void *context) {
    struct race *race = context;
    while (!atomic_load(&race->go)) {}
    for (unsigned i = 0; i < 1000; ++i) fire_irq();
    return NULL;
}
static void *poll_worker(void *context) {
    struct race *race = context;
    while (!atomic_load(&race->go)) {}
    for (unsigned i = 0; i < 1000; ++i) {
        unsigned mask = station_poll(race->file, NULL);
        assert(!mask || mask == (EPOLLIN | EPOLLRDNORM) || mask == (EPOLLERR | EPOLLHUP));
    }
    return NULL;
}
static void race_cases(void) {
    for (unsigned n = 0; n < 64; ++n) {
        struct platform_device p = device();
        struct inode inode = { MKDEV(42, 0) }; struct file file = {0};
        assert(!station_probe(&p) && !station_open(&inode, &file));
        struct race race = { .file = &file }; atomic_init(&race.go, false);
        pthread_t irq, poller, poller2;
        assert(!pthread_create(&irq, NULL, irq_worker, &race));
        assert(!pthread_create(&poller, NULL, poll_worker, &race));
        assert(!pthread_create(&poller2, NULL, poll_worker, &race));
        atomic_store(&race.go, true);
        assert(!station_remove(&p));
        assert(!pthread_join(irq, NULL) && !pthread_join(poller, NULL) && !pthread_join(poller2, NULL));
        assert(station_poll(&file, NULL) == (EPOLLERR | EPOLLHUP));
        assert(!station_release(&inode, &file)); clean();
    }
}
int main(void) {
    chrdev_error = -ENOMEM; assert(station_init() == -ENOMEM);
    assert(!chrdev_count && !class_count); chrdev_error = 0; ++cases;
    class_error = -ENOMEM; assert(station_init() == -ENOMEM);
    assert(!chrdev_count && !class_count); class_error = 0; ++cases;
    registration_error = -EINVAL; assert(station_init() == -EINVAL);
    assert(!chrdev_count && !class_count); registration_error = 0; ++cases;
    assert(!station_init()); gpio_result = 10; irq_result = 30;
    failure_cases(); reader_cases(); race_cases(); station_exit();
    assert(!chrdev_count && !class_count && !registered);
    printf("%u station lifecycle cases passed, including 64 IRQ/poll/removal races\n", cases);
    return 0;
}
