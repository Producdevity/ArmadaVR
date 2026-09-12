#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

struct test_fence {
    struct dma_fence base;
    spinlock_t lock;
};

struct signal_work {
    struct delayed_work work;
    struct dma_fence *fence;
};

static const char *test_name(struct dma_fence *fence)
{
    return "dmabuf-reservation-test";
}

static const struct dma_fence_ops test_ops = {
    .get_driver_name = test_name,
    .get_timeline_name = test_name,
    .release = dma_fence_free,
};

static struct dma_fence *new_fence(void)
{
    struct test_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);
    if (!f)
        return NULL;
    spin_lock_init(&f->lock);
    dma_fence_init(&f->base, &test_ops, &f->lock, dma_fence_context_alloc(1), 1);
    return &f->base;
}

static void signal_writer(struct work_struct *work)
{
    struct signal_work *signal = container_of(to_delayed_work(work), struct signal_work, work);
    dma_fence_signal(signal->fence);
}

static int exercise(bool wait)
{
    struct dma_fence *reader = new_fence(), *writer = new_fence();
    struct dma_resv reservation;
    struct signal_work signal;
    int ret = -ENOMEM;
    long remaining;

    if (!reader || !writer)
        goto release;
    dma_resv_init(&reservation);
    INIT_DELAYED_WORK(&signal.work, signal_writer);
    signal.fence = writer;
    dma_resv_lock(&reservation, NULL);
    ret = dma_resv_reserve_shared(&reservation, 1);
    if (!ret) {
        dma_resv_add_shared_fence(&reservation, reader);
        ret = dma_resv_add_excl_fence_preserve(&reservation, writer);
    }
    dma_resv_unlock(&reservation);
    if (ret)
        goto cleanup;
    ret = -EINVAL;
    if (wait) {
        schedule_delayed_work(&signal.work, msecs_to_jiffies(10));
        remaining = dma_resv_wait_timeout_rcu(&reservation, true, false, msecs_to_jiffies(100));
        cancel_delayed_work_sync(&signal.work);
        if (remaining != 0 || !dma_fence_is_signaled(writer) || dma_fence_is_signaled(reader))
            goto cleanup;
        dma_fence_signal(reader);
        if (dma_resv_wait_timeout_rcu(&reservation, true, false, 1) <= 0)
            goto cleanup;
        pr_info("DMA_BUF_RESV_WAIT_ALL_PASS\n");
    } else {
        dma_fence_signal(reader);
        if (dma_resv_test_signaled_rcu(&reservation, true) ||
            dma_resv_test_signaled_rcu(&reservation, false))
            goto cleanup;
        dma_fence_signal(writer);
        if (!dma_resv_test_signaled_rcu(&reservation, true))
            goto cleanup;
        pr_info("DMA_BUF_RESV_TEST_SIGNALED_PASS\n");
    }
    ret = 0;
cleanup:
    cancel_delayed_work_sync(&signal.work);
    dma_fence_signal(reader);
    dma_fence_signal(writer);
    dma_resv_fini(&reservation);
release:
    dma_fence_put(reader);
    dma_fence_put(writer);
    return ret;
}

static int __init test_start(void)
{
    int ret;
    if (!of_machine_is_compatible("linux,dummy-virt"))
        return -ENODEV;
    ret = exercise(false);
    if (!ret)
        ret = exercise(true);
    if (ret)
        pr_err("DMA_BUF_RESV_FAIL=%d\n", ret);
    return ret;
}

static void __exit test_stop(void) {}
module_init(test_start);
module_exit(test_stop);
MODULE_LICENSE("GPL");
