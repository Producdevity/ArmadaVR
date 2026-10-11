#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define __user
#define GFP_KERNEL 0
#define SPIDEV_MAJOR 153
#define N_SPI_MINORS 32
#define GPIOF_IN 1
#define GPIOF_OUT_INIT_LOW 2
#define IRQF_TRIGGER_RISING 1
#define ERESTARTSYS 512
#define POLLIN 1
#define POLLRDNORM 2
#define POLLERR 4
#define POLLHUP 8
#define SPI_IOC_SET_RFIC_IO2_STATUS 1
#define SPI_IOC_SET_RFIC_IO3_STATUS 2
#define SPI_IOC_SET_POWER_STATUS 3
#define MKDEV(major, minor) (minor)
#define MINOR(dev) (dev)
#define READ_ONCE(value) (value)
#define PTR_ERR_OR_ZERO(value) ((intptr_t)(value) < 0 ? (int)(intptr_t)(value) : 0)
#define dev_dbg(...) ((void)0)
#define pr_debug(...) ((void)0)
typedef unsigned char u8;
typedef uint32_t __u32;
typedef int poll_table;
typedef long long loff_t;
struct device_node { int present; };
struct device { struct device_node *of_node; };
struct spi_master { int bus_num; };
struct spi_device { struct device dev; struct spi_master *master; int chip_select; unsigned max_speed_hz; void *data; };
struct spi_transfer { const void *tx_buf; void *rx_buf; unsigned len, speed_hz; };
struct spi_message { struct spi_transfer *transfer; unsigned actual_length; };
struct list_head { bool linked; };
struct spidev_data {
    unsigned devt; int spi_lock; struct spi_device *spi; struct list_head device_entry;
    pthread_mutex_t buf_lock; unsigned users; u8 *tx_buffer, *rx_buffer; unsigned speed_hz;
    int data_gpio, data_irq, rfic_io2_gpio, rfic_io3_gpio;
};
struct inode { int unused; };
struct file { struct spidev_data *private_data; };
struct completion { unsigned done; };
static struct spidev_data *pvr_dev;
static pthread_mutex_t device_list_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list_head device_list;
static unsigned long minors[1];
static const unsigned bufsiz = 4096;
static int spidev_class, spidev_dt_ids, controller_state, dtry_miss, spi_status, poll_ioctl, spi_process_list;
static struct completion spi_completion;
static int operation, fail_at, failure_code, allocations, irq_count, nodes, gpio_writes, power_writes;
static int requested[3], request_flags[3];
static void *irq_cookie;
static bool no_minor, copy_fault;
static int interrupted, transfer_error, transfers;
static const void *expected_tx;
static unsigned expected_count;
static pthread_mutex_t transfer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t transfer_condition = PTHREAD_COND_INITIALIZER;
static bool block_transfer, transfer_started, allow_transfer, remove_started, remove_done;

static bool fail(void) { return operation++ == fail_at; }
static void mutex_init(pthread_mutex_t *lock) { assert(!pthread_mutex_init(lock, NULL)); }
static void mutex_lock(pthread_mutex_t *lock) { assert(!pthread_mutex_lock(lock)); }
static void mutex_unlock(pthread_mutex_t *lock) { assert(!pthread_mutex_unlock(lock)); }
static void spin_lock_init(int *lock) { *lock = 0; }
static void spin_lock_irq(int *lock) { assert(!*lock); *lock = 1; }
static void spin_unlock_irq(int *lock) { assert(*lock); *lock = 0; }
static void atomic_set(int *p, int v) { *p = v; }
static int atomic_read(int *p) { return *p; }
static void reinit_completion(struct completion *c) { c->done = 0; }
static void complete_all(struct completion *c) { c->done = ~0u; }
static int wait_for_completion_interruptible(struct completion *c) {
    if (interrupted) return -ERESTARTSYS;
    assert(c->done); return 0;
}
static void wake_up_interruptible(int *p) { (void)p; }
static void poll_wait(struct file *f, int *p, poll_table *t) { (void)f; (void)p; (void)t; }
static void *kzalloc(size_t n, int flags) {
    (void)flags; if (fail()) return NULL; allocations++; return calloc(1, n);
}
static void kfree(void *p) {
    if (!p) return;
    if (p == irq_cookie) assert(!irq_count);
    allocations--; free(p);
}
static int of_match_device(int ids, struct device *dev) { (void)ids; return dev->of_node->present; }
static void spidev_probe_acpi(struct spi_device *spi) { (void)spi; }
static void INIT_LIST_HEAD(struct list_head *list) { list->linked = false; }
static void list_add(struct list_head *list, struct list_head *head) { (void)head; assert(!list->linked); list->linked = true; }
static void list_del(struct list_head *list) { assert(list->linked); list->linked = false; }
static unsigned long find_first_zero_bit(unsigned long *bits, int n) { return no_minor ? (unsigned)n : (*bits ? 1 : 0); }
static void set_bit(unsigned long bit, unsigned long *bits) { *bits |= 1ul << bit; }
static void clear_bit(unsigned long bit, unsigned long *bits) { *bits &= ~(1ul << bit); }
static int of_get_named_gpio(struct device_node *np, const char *name, int index) {
    (void)np; (void)index; if (fail()) return failure_code;
    return !strcmp(name, "gpio") ? 10 : !strcmp(name, "rfic-io2-gpio") ? 11 : 12;
}
static int gpio_request_one(int gpio, int flags, const char *name) {
    (void)name; if (fail()) return failure_code;
    assert(gpio >= 10 && gpio <= 12 && !requested[gpio-10]);
    requested[gpio-10] = 1; request_flags[gpio-10] = flags; return 0;
}
static void gpio_free(int gpio) { assert(gpio >= 10 && gpio <= 12 && requested[gpio-10]); requested[gpio-10] = 0; }
static int gpio_to_irq(int gpio) { assert(gpio == 10 && requested[0]); return fail() ? failure_code : 27; }
static int ultrasonic_interrupt(int irq, void *data) { (void)irq; (void)data; return 0; }
static int request_irq(int irq, int (*handler)(int, void *), int flags, const char *name, void *cookie) {
    (void)handler; (void)flags; (void)name;
    assert(irq == 27 && cookie && !irq_count);
    struct spidev_data *state = cookie;
    assert(state->spi && state->speed_hz && !state->spi_lock);
    assert(requested[0] && requested[1] && requested[2]);
    assert(request_flags[0] == GPIOF_IN && request_flags[1] == GPIOF_OUT_INIT_LOW && request_flags[2] == GPIOF_OUT_INIT_LOW);
    if (fail()) return failure_code;
    irq_cookie = cookie; irq_count = 1; return 0;
}
static void free_irq(int irq, void *cookie) { assert(irq == 27 && irq_count == 1 && cookie == irq_cookie); irq_count = 0; irq_cookie = NULL; }
static struct device *device_create(int cls, struct device *parent, unsigned devt, void *data, const char *format, ...) {
    (void)cls; (void)parent; (void)devt; (void)data; (void)format;
    if (fail()) return (void *)(intptr_t)failure_code;
    nodes++; return parent;
}
static void device_destroy(int cls, unsigned devt) { (void)cls; (void)devt; assert(nodes == 1); nodes--; }
static void spi_set_drvdata(struct spi_device *spi, void *p) { spi->data = p; }
static void *spi_get_drvdata(struct spi_device *spi) { return spi->data; }
static void spi_message_init(struct spi_message *m) { memset(m, 0, sizeof(*m)); }
static void spi_message_add_tail(struct spi_transfer *t, struct spi_message *m) { m->transfer = t; }
static int spi_sync(struct spi_device *spi, struct spi_message *m) {
    assert(spi && m->transfer->speed_hz == spi->max_speed_hz);
    if (expected_tx) { assert(m->transfer->tx_buf == expected_tx); assert(m->transfer->len == expected_count); }
    mutex_lock(&transfer_lock);
    if (block_transfer) {
        transfer_started = true; pthread_cond_broadcast(&transfer_condition);
        while (!allow_transfer) pthread_cond_wait(&transfer_condition, &transfer_lock);
    }
    mutex_unlock(&transfer_lock);
    transfers++;
    if (transfer_error) return transfer_error;
    if (m->transfer->rx_buf) memset(m->transfer->rx_buf, 42, m->transfer->len);
    m->actual_length = m->transfer->len; return 0;
}
static unsigned long copy_from_user(void *dst, const void *src, size_t n) { if (copy_fault) return n; memcpy(dst, src, n); return 0; }
static unsigned long copy_to_user(void *dst, const void *src, size_t n) { return copy_from_user(dst, src, n); }
#define get_user(dst, src) (copy_fault ? -EFAULT : ((dst) = *(src), 0))
static void gpio_set_value(int gpio, int v) { (void)v; assert(requested[gpio-10]); gpio_writes++; }
static int spidev_mcu_power_on(void) { power_writes++; return 0; }
static int spidev_mcu_power_off(void) { power_writes++; return 0; }

#include "driver-functions.inc"

static void clean(void) {
    assert(!pvr_dev && !allocations && !irq_count && !nodes && !minors[0]);
    assert(!requested[0] && !requested[1] && !requested[2]);
}
static void reset(void) {
    clean(); operation = 0; fail_at = -1; failure_code = -EIO;
    no_minor = copy_fault = false; interrupted = transfer_error = transfers = 0;
    expected_tx = NULL; block_transfer = transfer_started = allow_transfer = remove_started = remove_done = false;
}
static void *camera_thread(void *arg) { assert(cam_spidev_write(arg, 8) == 8); return NULL; }
static void *remove_thread(void *arg) {
    mutex_lock(&transfer_lock); remove_started = true; pthread_cond_broadcast(&transfer_condition); mutex_unlock(&transfer_lock);
    assert(!spidev_remove(arg));
    mutex_lock(&transfer_lock); remove_done = true; pthread_cond_broadcast(&transfer_condition); mutex_unlock(&transfer_lock);
    return NULL;
}
int main(void) {
    struct device_node node = { .present = 1 };
    struct spi_master master = { .bus_num = 0 };
    struct spi_device spi = { .dev.of_node = &node, .master = &master, .max_speed_hz = 1000000 };
    struct spi_device second = spi;
    unsigned cases = 0;
    const int errors[] = { -EIO, -ENOMEM, -EBUSY, -517 };
    reset(); assert(!spidev_probe(&spi)); int operations = operation;
    assert(operations == 10); assert(!spidev_remove(&spi)); clean(); cases++;
    for (unsigned e = 0; e < sizeof(errors)/sizeof(errors[0]); e++) {
        for (int n = 0; n < operations; n++) {
            reset(); fail_at = n; failure_code = errors[e];
            assert(spidev_probe(&spi) == (n == 0 ? -ENOMEM : failure_code));
            assert(operation == n + 1); assert(!spi.data); clean(); cases++;
        }
    }
    reset(); no_minor = true; assert(spidev_probe(&spi) == -ENODEV); clean(); cases++;
    reset(); node.present = 0; assert(spidev_probe(&spi) == -ENODEV); node.present = 1; clean(); cases++;
    reset(); unsigned char data[8] = {1,2,3,4,5,6,7,8};
    assert(cam_spidev_write(data, sizeof(data)) == -ENODEV);
    assert(cam_spidev_write(data, -1) == -EMSGSIZE);
    assert(cam_spidev_write(data, bufsiz+1) == -EMSGSIZE);
    assert(cam_spidev_write(NULL, 1) == -EINVAL); cases += 4;
    assert(!spidev_probe(&spi)); assert(spidev_probe(&second) == -EBUSY); assert(!second.data);
    struct spidev_data *state = pvr_dev;
    unsigned char owned[8] = {0}, received[8] = {0};
    state->tx_buffer = owned; state->rx_buffer = received;
    struct file file = { .private_data = state };
    expected_tx = data; expected_count = sizeof(data);
    assert(cam_spidev_write(data, sizeof(data)) == sizeof(data)); assert(state->tx_buffer == owned);
    transfer_error = -EIO; assert(cam_spidev_write(data, sizeof(data)) == -EIO); transfer_error = 0;
    expected_tx = NULL; cases += 3;
    copy_fault = true;
    for (unsigned cmd = 1; cmd <= 3; cmd++) assert(controls(cmd, (unsigned long)NULL) == -EFAULT);
    assert(!gpio_writes && !power_writes);
    assert(spidev_write(&file, (char *)data, sizeof(data), NULL) == -EFAULT);
    copy_fault = false;
    unsigned value = 1;
    for (unsigned cmd = 1; cmd <= 3; cmd++) assert(!controls(cmd, (unsigned long)&value));
    assert(gpio_writes == 2 && power_writes == 1); cases += 7;
    interrupted = 1; assert(spidev_read(&file, (char *)data, sizeof(data), NULL) == -ERESTARTSYS); interrupted = 0;
    complete_all(&spi_completion); assert(spidev_read(&file, (char *)data, sizeof(data), NULL) == sizeof(data));
    assert(data[0] == 42); cases += 2;
    struct file second_file = { .private_data = state };
    state->users = 2; assert(!spidev_remove(&spi));
    assert(pvr_dev == state && !state->spi && !spi.data && spi_completion.done);
    assert(cam_spidev_write(data, sizeof(data)) == -ENODEV);
    assert(spidev_read(&file, (char *)data, sizeof(data), NULL) == -ESHUTDOWN);
    assert(spidev_write(&file, (char *)data, sizeof(data), NULL) == -ESHUTDOWN);
    assert(spi_completion.done); assert(spi_poll(&file, NULL) == (POLLERR | POLLHUP));
    assert(spidev_probe(&second) == -EBUSY);
    state->tx_buffer = state->rx_buffer = NULL;
    assert(!spidev_release(NULL, &file));
    assert(pvr_dev == state && state->users == 1);
    assert(spidev_probe(&second) == -EBUSY);
    assert(!spidev_release(NULL, &second_file)); clean(); cases += 10;
    for (int iteration = 0; iteration < 64; iteration++) {
        reset(); assert(!spidev_probe(&spi)); block_transfer = true;
        pthread_t writer, remover;
        assert(!pthread_create(&writer, NULL, camera_thread, data));
        mutex_lock(&transfer_lock);
        while (!transfer_started) pthread_cond_wait(&transfer_condition, &transfer_lock);
        assert(!pthread_create(&remover, NULL, remove_thread, &spi));
        while (!remove_started) pthread_cond_wait(&transfer_condition, &transfer_lock);
        assert(!remove_done && pvr_dev && irq_count == 1);
        allow_transfer = true; pthread_cond_broadcast(&transfer_condition); mutex_unlock(&transfer_lock);
        pthread_join(writer, NULL); pthread_join(remover, NULL); assert(remove_done); clean(); cases++;
    }
    printf("Controller lifecycle PASS: %u cases, including 64 concurrent write/remove cycles\n", cases);
}
