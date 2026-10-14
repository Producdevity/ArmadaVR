// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EPROBE_DEFER 517
#define PHY_HOST_MODE 1
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 5
#define DWC3_GCTL_PRTCAP_HOST 1
#define DWC3_GCTL_PRTCAP_DEVICE 2
#define DWC3_USB31_REVISION_170A 0x170a
#define DWC31_LINK_LU3LFPSRXTIM(n) (n)
#define GEN2_U3_EXIT_RSP_RX_CLK_MASK 1
#define GEN1_U3_EXIT_RSP_RX_CLK_MASK 2
#define PM_QOS_CPU_DMA_LATENCY 1
#define PM_QOS_DEFAULT_VALUE 0
#define PM_QOS_SAMPLE_SEC 1
#define WAIT_FOR_LPM 0
#define CONFIG_SMP 1
#define PM_QOS_REQ_AFFINE_IRQ 1
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define unlikely(x) (x)
#define rcu_assign_pointer(p, n) ((p) = (n))
#define WARN(...) (++warnings)
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define dbg_event(...) ((void)0)

struct notifier_block {
    int (*notifier_call)(struct notifier_block *, unsigned long, void *);
    struct notifier_block *next;
    int priority;
};
static int warnings;
#include "notifier.inc"
static struct notifier_block *atomic_chain, *blocking_chain;
static int callbacks;
static int msm_dwc3_usbdev_notify(struct notifier_block *n, unsigned long a, void *p)
{ (void)n; (void)a; (void)p; ++callbacks; return 0; }
static int dwc3_msm_host_notifier(struct notifier_block *n, unsigned long a, void *p)
{ (void)n; (void)a; (void)p; return 0; }
static void usb_register_notify(struct notifier_block *n)
{ notifier_chain_register(&blocking_chain, n); }
static void usb_unregister_notify(struct notifier_block *n)
{ assert(notifier_chain_unregister(&blocking_chain, n) == 0); }
static void usb_register_atomic_notify(struct notifier_block *n)
{ notifier_chain_register(&atomic_chain, n); }
static void usb_unregister_atomic_notify(struct notifier_block *n)
{ assert(notifier_chain_unregister(&atomic_chain, n) == 0); }

struct device { int refs; };
struct regulator { int refs; };
struct usb_phy { unsigned flags; bool connected; };
struct dwc3 { int maximum_speed, revision, irq, active; };
struct dwc3_msm {
    struct dwc3 *dwc3;
    struct device *dev;
    struct regulator *vbus_reg;
    struct usb_phy *hs_phy, *ss_phy;
    struct notifier_block host_nb, usbdev_nb;
    bool in_host_mode;
    void *base;
    struct { int type, irq, active; } pm_qos_req_dma;
    int perf_vote_work;
    unsigned long inputs;
};
static struct regulator *lookup_reg;
static int init_error, enable_error, init_calls, exits;
static int resume_result, unsafe_access, idle_puts, noidle_puts;
static bool powered;
static void hardware_access(void) { if (!powered) ++unsafe_access; }
static struct dwc3 *platform_get_drvdata(struct dwc3 *d) { return d; }
static struct regulator *devm_regulator_get_optional(struct device *d, const char *n)
{ (void)d; (void)n; return lookup_reg; }
static int regulator_enable(struct regulator *r)
{ if (enable_error) return enable_error; ++r->refs; return 0; }
static int regulator_disable(struct regulator *r)
{ assert(r->refs > 0); --r->refs; return 0; }
static int pm_runtime_get_sync(struct device *d)
{ ++d->refs; powered = resume_result >= 0; return resume_result; }
static void pm_runtime_put_sync(struct device *d)
{ assert(d->refs > 0); --d->refs; ++idle_puts; }
#define pm_runtime_put_noidle(d) do { assert((d)->refs > 0); --(d)->refs; ++noidle_puts; } while (0)
#define pm_runtime_put_sync_autosuspend pm_runtime_put_sync
#define pm_runtime_put_sync_suspend pm_runtime_put_sync
static void pm_runtime_mark_last_busy(struct device *d) { (void)d; }
static void usb_phy_notify_connect(struct usb_phy *p, int speed) { hardware_access(); p->connected = true; (void)speed; }
static void usb_phy_notify_disconnect(struct usb_phy *p, int speed) { hardware_access(); p->connected = false; (void)speed; }
static void dwc3_set_prtcap(struct dwc3 *d, int mode) { hardware_access(); (void)d; (void)mode; }
static void dwc3_en_sleep_mode(struct dwc3 *d) { (void)d; }
static int dwc3_host_init(struct dwc3 *d)
{ hardware_access(); ++init_calls; assert(!d->active); if (!init_error) d->active = 1; return init_error; }
static void dwc3_host_exit(struct dwc3 *d) { assert(d->active); d->active = 0; ++exits; }
static void dwc3_usb3_phy_suspend(struct dwc3 *d, bool on) { (void)d; (void)on; }
static void dwc3_msm_write_reg_field(void *b, int reg, int mask, int value)
{ (void)b; (void)reg; (void)mask; (void)value; }
#define pm_qos_add_request(q, kind, value) ((q)->active = 1)
#define pm_qos_remove_request(q) do { assert((q)->active); (q)->active = 0; } while (0)
static void msm_dwc3_perf_vote_update(struct dwc3_msm *m, bool on) { (void)m; (void)on; }
static void schedule_delayed_work(int *work, int delay) { (void)delay; *work = 1; }
static void cancel_delayed_work_sync(int *work) { *work = 0; }
static int msecs_to_jiffies(int ms) { return ms; }
static void set_bit(int bit, unsigned long *value) { *value |= 1UL << bit; }
#include "drivers/usb/dwc3/dwc3-msm.c"

static int defects, cases;
static void expect(bool condition) { if (!condition) ++defects; }
static bool contains(struct notifier_block *head, struct notifier_block *n)
{
    for (int limit = 0; head; head = head->next) {
        assert(++limit < 4);
        if (head == n) return true;
    }
    return false;
}
static void notify_all(void)
{
    for (struct notifier_block *n = atomic_chain; n; n = n->next)
        n->notifier_call(n, 0, NULL);
}

int main(void)
{
    const int errors[] = {-ENOMEM, -ENODEV, -EPROBE_DEFER, -EIO};
    for (int speed = 0; speed < 2; ++speed)
    for (int revision = 0; revision < 2; ++revision)
    for (int optional = 0; optional < 2; ++optional)
    for (unsigned error = 0; error < sizeof(errors) / sizeof(errors[0]); ++error)
    for (int repetition = 0; repetition < 8; ++repetition)
    for (int already_active = 0; already_active < 2; ++already_active) {
        struct device dev = {0};
        struct regulator reg = {0};
        struct usb_phy hs = {0}, ss = {0};
        struct dwc3 dwc = {.maximum_speed = speed ? USB_SPEED_SUPER : USB_SPEED_HIGH,
                          .revision = revision ? DWC3_USB31_REVISION_170A : 0};
        struct dwc3_msm m = {.dwc3 = &dwc, .dev = &dev, .hs_phy = &hs, .ss_phy = &ss};
        struct notifier_block other = {.notifier_call = msm_dwc3_usbdev_notify, .priority = 1};
        assert(!atomic_chain && !blocking_chain);
        usb_register_atomic_notify(&other);
        lookup_reg = optional ? (struct regulator *)(intptr_t)-ENODEV : &reg;
        init_error = errors[error]; enable_error = 0; warnings = 0; init_calls = exits = 0;
        resume_result = already_active; unsafe_access = idle_puts = noidle_puts = 0;
        for (int retry = 0; retry < 3; ++retry) {
            assert(dwc3_otg_start_host(&m, 1) == init_error);
            expect(!contains(atomic_chain, &m.usbdev_nb));
            expect(!hs.connected && !ss.connected);
            assert(!contains(blocking_chain, &m.host_nb));
            assert(!m.in_host_mode && !dwc.active && !dev.refs && !reg.refs);
            assert(!(hs.flags & PHY_HOST_MODE) && !(ss.flags & PHY_HOST_MODE));
            callbacks = 0; notify_all(); expect(callbacks == 1);
        }
        init_error = 0;
        assert(dwc3_otg_start_host(&m, 1) == 0);
        expect(!warnings);
        assert(m.in_host_mode && dwc.active && !dev.refs);
        assert(contains(atomic_chain, &m.usbdev_nb) && contains(blocking_chain, &m.host_nb));
        callbacks = 0; notify_all(); assert(callbacks == 2);
        assert(dwc3_otg_start_host(&m, 0) == 0);
        assert(!m.in_host_mode && !dwc.active && !dev.refs && !reg.refs);
        assert(!m.perf_vote_work && !m.pm_qos_req_dma.active);
        assert(atomic_chain == &other && !other.next && !blocking_chain);
        assert(init_calls == 4 && exits == 1);
        usb_unregister_atomic_notify(&other);

        m.vbus_reg = NULL; lookup_reg = (struct regulator *)(intptr_t)-EPROBE_DEFER;
        assert(dwc3_otg_start_host(&m, 1) == -EPROBE_DEFER && !m.vbus_reg);
        assert(!atomic_chain && !blocking_chain && init_calls == 4);
        lookup_reg = &reg; enable_error = -EIO;
        assert(dwc3_otg_start_host(&m, 1) == -EIO);
        assert(!atomic_chain && !blocking_chain && init_calls == 4 && !dev.refs && !reg.refs);
        expect(!hs.connected && !ss.connected);
        expect(!unsafe_access && !noidle_puts);
        ++cases;
    }
    const int resume_errors[] = {-EINVAL, -EACCES, -EIO};
    for (int speed = 0; speed < 2; ++speed)
    for (int optional = 0; optional < 2; ++optional)
    for (unsigned error = 0; error < sizeof(resume_errors) / sizeof(resume_errors[0]); ++error)
    for (int repetition = 0; repetition < 8; ++repetition) {
        struct device dev = {0};
        struct regulator reg = {0};
        struct usb_phy hs = {0}, ss = {0};
        struct dwc3 dwc = {.maximum_speed = speed ? USB_SPEED_SUPER : USB_SPEED_HIGH};
        struct dwc3_msm m = {.dwc3 = &dwc, .dev = &dev, .hs_phy = &hs, .ss_phy = &ss};
        struct notifier_block other = {.notifier_call = msm_dwc3_usbdev_notify, .priority = 1};
        assert(!atomic_chain && !blocking_chain);
        usb_register_atomic_notify(&other);
        lookup_reg = optional ? (struct regulator *)(intptr_t)-ENODEV : &reg;
        init_error = enable_error = init_calls = exits = 0;
        unsafe_access = idle_puts = noidle_puts = 0;
        resume_result = resume_errors[error];
        expect(dwc3_otg_start_host(&m, 1) == resume_result);
        expect(!init_calls && !unsafe_access);
        expect(!m.in_host_mode && !dwc.active && !dev.refs && !reg.refs);
        expect(!hs.connected && !ss.connected && !hs.flags && !ss.flags);
        expect(atomic_chain == &other && !other.next && !blocking_chain);
        expect(!idle_puts && noidle_puts == 1);
        if (m.in_host_mode) {
            resume_result = 0;
            assert(dwc3_otg_start_host(&m, 0) == 0);
        }
        usb_unregister_atomic_notify(&other);
        resume_result = repetition % 2;
        assert(dwc3_otg_start_host(&m, 1) == 0);
        assert(dwc3_otg_start_host(&m, 0) == 0);
        assert(!atomic_chain && !blocking_chain && !hs.connected && !ss.connected);
        assert(!dev.refs && !reg.refs && !m.in_host_mode && !dwc.active);
        ++cases;
    }
    printf("PICO_USB_HOST cases=%d defects=%d\n", cases, defects);
    return defects ? 1 : 0;
}
