// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CAM_DBG(...) ((void)0)
#define CAM_ERR(...) ((void)0)
#define CAM_WARN(...) ((void)0)
#define CAM_ISP_HW_SPLIT_MAX 2
#define CAM_ISP_HW_SPLIT_LEFT 0
#define CAM_IFE_PIX_PATH_RES_MAX 8
#define CAM_IFE_HW_OUT_RES_MAX 2
#define CAM_ISP_RESOURCE_STATE_STREAMING 3
#define CAM_ISP_RESOURCE_PIX_PATH 1
#define CAM_IFE_PIX_PATH_RES_IPP 0
#define CAM_IFE_HW_MGR_RES_IFE_OUT 4
#define CAM_ISP_HW_CMD_STOP_BUS_ERR_IRQ 1
#define CAM_ISP_HW_CMD_CSID_CHANGE_HALT_MODE 2
#define CAM_ISP_HW_STOP_AT_FRAME_BOUNDARY 1
#define CAM_ISP_HW_STOP_IMMEDIATELY 2
#define CAM_IFE_SAFE_DISABLE 0
#define CAM_SYNC_STATE_SIGNALED_ERROR 2
#define CAM_ISP_CTX_ACTIVATED_HALT 9
#define CAM_ISP_CTX_EVENT_MAX 4
#define CAM_CTX_ACQUIRED 2

enum cam_ife_csid_halt_mode { CAM_CSID_HALT_MODE_INTERNAL };
enum cam_ife_csid_halt_cmd { CAM_CSID_HALT_AT_FRAME_BOUNDARY, CAM_CSID_HALT_IMMEDIATELY };
struct list_head { struct list_head *next, *prev; };
#define INIT_LIST_HEAD(h) ((h)->next = (h)->prev = (h))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define list_first_entry(h, t, m) container_of((h)->next, t, m)
#define list_for_each_entry(p, h, m) \
    for (struct list_head *it = (h)->next; it != (h) && ((p) = container_of(it, __typeof__(*(p)), m), 1); it = it->next)
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{ n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n; }
static void list_del_init(struct list_head *n)
{ n->prev->next = n->next; n->next->prev = n->prev; INIT_LIST_HEAD(n); }
#define spin_lock_bh(p) ((void)(p))
#define spin_unlock_bh(p) ((void)(p))
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define atomic_set(p, n) (*(p) = (n))
#define atomic64_set atomic_set
#define atomic_dec_return(p) (--*(p))
#define msecs_to_jiffies(n) (n)
#define trace_cam_context_state(n, p) ((void)(p))

struct cam_hw_intf {
    unsigned hw_idx;
    void *hw_priv;
    struct {
        int (*stop)(void *, void *, uint32_t);
        int (*process_cmd)(void *, uint32_t, void *, uint32_t);
    } hw_ops;
};
struct cam_isp_resource_node { int res_state, res_type, res_id; struct cam_hw_intf *hw_intf; };
struct cam_ife_hw_mgr_res {
    struct cam_isp_resource_node *hw_res[2];
    int res_type;
    struct list_head list;
};
struct cam_csid_hw_halt_args { struct cam_isp_resource_node *node_res; int halt_mode; };
struct cam_csid_hw_stop_args { unsigned num_res; struct cam_isp_resource_node **node_res; unsigned stop_cmd; };
struct cam_ife_hw_mgr_ctx {
    bool ctx_in_use, dsp_enabled;
    unsigned num_base, ctx_index;
    struct { unsigned idx, split_id; } base[2];
    struct list_head res_list_ife_csid, res_list_ife_cid, res_list_ife_src, res_list_ife_in_rd;
    struct cam_ife_hw_mgr_res res_list_ife_out[CAM_IFE_HW_OUT_RES_MAX];
    struct { int tasklet_info; } common;
    int config_done_complete, cdm_handle;
    uint64_t applied_req_id;
};
struct cam_isp_stop_args { int hw_stop_cmd; bool stop_only; };
struct cam_hw_stop_args { void *ctxt_to_hw_map, *args; };
static struct { int ctx_mutex, active_ctx_cnt; } g_ife_hw_mgr;

enum stage { HALT_MODE, CSID_STOP, IFE_STOP, BUS_IRQ, TASKLET, PAUSE, CONFIG_DONE, CDM_OFF, DEINIT, SCM };
static enum stage events[64];
static int count, fail_at, second_fail, fault_calls, signals, notifications, unlinks;
static int unlink_error;
static int event(enum stage s)
{
    assert(count < 64);
    events[count] = s;
    int at = count++;
    if (at == fail_at) { ++fault_calls; return -EIO; }
    if (at == second_fail) { ++fault_calls; return -ENOSPC; }
    return 0;
}
static int stop_resource(void *p, void *arg, uint32_t size)
{
    (void)p; assert(arg);
    assert(size == sizeof(struct cam_isp_resource_node));
    return event(IFE_STOP);
}
static int stop_csid(void *p, void *arg, uint32_t size)
{
    (void)p; assert(size == sizeof(struct cam_csid_hw_stop_args));
    struct cam_csid_hw_stop_args *s = arg;
    assert(s->num_res && s->num_res < CAM_IFE_PIX_PATH_RES_MAX);
    return event(CSID_STOP);
}
static int process(void *p, uint32_t cmd, void *arg, uint32_t size)
{
    (void)p; assert(arg && size);
    assert(cmd == CAM_ISP_HW_CMD_STOP_BUS_ERR_IRQ || cmd == CAM_ISP_HW_CMD_CSID_CHANGE_HALT_MODE);
    return event(cmd == CAM_ISP_HW_CMD_STOP_BUS_ERR_IRQ ? BUS_IRQ : HALT_MODE);
}
static void cam_tasklet_stop(int p) { (void)p; assert(!event(TASKLET)); }
static int cam_ife_mgr_pause_hw(struct cam_ife_hw_mgr_ctx *p) { assert(p); return event(PAUSE); }
static unsigned long wait_for_completion_timeout(int *p, int timeout)
{ assert(p && timeout == 10); return event(CONFIG_DONE) ? 0 : 1; }
static int cam_cdm_stream_off(int handle) { assert(handle == 7); return event(CDM_OFF); }
static void cam_ife_hw_mgr_deinit_hw(struct cam_ife_hw_mgr_ctx *p) { assert(p); assert(!event(DEINIT)); }
static int cam_ife_notify_safe_lut_scm(int action) { assert(action == CAM_IFE_SAFE_DISABLE); return event(SCM); }
#include "techpack/camera/drivers/cam_isp/isp_hw_mgr/cam_ife_hw_mgr.c"

struct cam_start_stop_dev_cmd { int unused; };
struct cam_isp_ctx_req { unsigned num_fence_map_out; struct { int sync_id; } fence_map_out[2]; };
struct cam_ctx_request { void *req_priv; struct list_head list; };
struct cam_req_mgr_notify_stop { int link_hdl; };
struct cam_isp_context {
    void *hw_ctx;
    bool init_received;
    int substate_activated, frame_id, active_req_cnt, reported_req_id, bubble_frame_cnt, last_applied_req_id;
    struct { int last_bufdone_req_id; } req_info;
    int process_bubble, rxd_epoch;
    int64_t state_monitor_head, event_record_head[CAM_ISP_CTX_EVENT_MAX];
};
struct hw_interface { void *hw_mgr_priv; int (*hw_stop)(void *, void *); };
struct crm_interface { int (*notify_stop)(struct cam_req_mgr_notify_stop *); };
struct cam_context {
    void *ctx_priv;
    struct hw_interface *hw_mgr_intf;
    struct crm_interface *ctx_crm_intf;
    int lock, ctx_id, link_hdl, state;
    struct list_head pending_req_list, wait_req_list, active_req_list, free_req_list;
};
static int notify_stop(struct cam_req_mgr_notify_stop *n)
{ assert(n->link_hdl == 19); ++notifications; return 0; }
static int cam_sync_signal(int id, int state)
{ assert(id >= 10 && id <= 12 && state == CAM_SYNC_STATE_SIGNALED_ERROR); ++signals; return 0; }
static int __cam_isp_ctx_unlink_in_ready(struct cam_context *ctx, void *args)
{ assert(ctx && !args); ++unlinks; return unlink_error; }
#include "techpack/camera/drivers/cam_isp/cam_isp_context.c"

struct fixture {
    struct cam_ife_hw_mgr_ctx hw;
    struct cam_hw_intf csid[2], ife[2];
    struct cam_isp_resource_node nodes[6][2];
    struct cam_ife_hw_mgr_res csid_paths, cid_paths, src, rd;
    struct cam_context context;
    struct cam_isp_context isp;
    struct hw_interface hw_intf;
    struct crm_interface crm_intf;
    struct cam_ctx_request requests[3];
    struct cam_isp_ctx_req request_data[3];
};
static void setup(struct fixture *f, int active)
{
    memset(f, 0, sizeof(*f)); count = fault_calls = signals = notifications = unlinks = 0;
    g_ife_hw_mgr.active_ctx_cnt = active;
    f->hw.ctx_in_use = true; f->hw.num_base = 2; f->hw.cdm_handle = 7;
    struct cam_ife_hw_mgr_res *resources[] = {&f->csid_paths, &f->cid_paths,
        &f->hw.res_list_ife_out[0], &f->hw.res_list_ife_out[1], &f->src, &f->rd};
    struct list_head *lists[] = {&f->hw.res_list_ife_csid, &f->hw.res_list_ife_cid,
        &f->hw.res_list_ife_src, &f->hw.res_list_ife_in_rd};
    for (unsigned i = 0; i < 4; ++i) INIT_LIST_HEAD(lists[i]);
    for (unsigned side = 0; side < 2; ++side) {
        f->hw.base[side].idx = side; f->hw.base[side].split_id = side;
        f->csid[side] = (struct cam_hw_intf){.hw_idx=side, .hw_ops={stop_csid, process}};
        f->ife[side] = (struct cam_hw_intf){.hw_idx=side, .hw_ops={stop_resource, process}};
        for (unsigned i = 0; i < 6; ++i) {
            f->nodes[i][side] = (struct cam_isp_resource_node){.res_state=CAM_ISP_RESOURCE_STATE_STREAMING,
                .res_type=CAM_ISP_RESOURCE_PIX_PATH, .res_id=CAM_IFE_PIX_PATH_RES_IPP,
                .hw_intf=i < 2 ? &f->csid[side] : &f->ife[side]};
            resources[i]->hw_res[side] = &f->nodes[i][side];
            resources[i]->res_type = i == 2 || i == 3 ? CAM_IFE_HW_MGR_RES_IFE_OUT : 0;
        }
    }
    list_add_tail(&f->csid_paths.list, lists[0]); list_add_tail(&f->cid_paths.list, lists[1]);
    list_add_tail(&f->src.list, lists[2]); list_add_tail(&f->rd.list, lists[3]);
    f->hw_intf = (struct hw_interface){&g_ife_hw_mgr, cam_ife_mgr_stop_hw};
    f->crm_intf.notify_stop = notify_stop;
    f->isp.hw_ctx = &f->hw; f->isp.init_received = true; f->isp.active_req_cnt = 1;
    f->context.ctx_priv = &f->isp; f->context.hw_mgr_intf = &f->hw_intf;
    f->context.ctx_crm_intf = &f->crm_intf; f->context.link_hdl = 19;
    struct list_head *requests[] = {&f->context.pending_req_list, &f->context.wait_req_list,
        &f->context.active_req_list};
    INIT_LIST_HEAD(&f->context.free_req_list);
    for (unsigned i = 0; i < 3; ++i) {
        INIT_LIST_HEAD(requests[i]);
        f->request_data[i].num_fence_map_out = 2;
        f->request_data[i].fence_map_out[0].sync_id = 10 + i;
        f->request_data[i].fence_map_out[1].sync_id = -1;
        f->requests[i].req_priv = &f->request_data[i];
        list_add_tail(&f->requests[i].list, requests[i]);
    }
}
static int defects, cases;
static void expect(bool condition) { if (!condition) ++defects; }
static bool injectable(enum stage s) { return s != TASKLET && s != DEINIT; }
static int expected(enum stage s, bool second) { return s == SCM ? 0 : s == CONFIG_DONE ? -ETIMEDOUT : second ? -ENOSPC : -EIO; }
static int invoke(struct fixture *f, int mode)
{
    struct cam_start_stop_dev_cmd cmd = {0};
    if (mode == 0) return __cam_isp_ctx_stop_dev_in_activated(&f->context, &cmd);
    if (mode == 1) return __cam_isp_ctx_stop_dev_in_activated_unlock(&f->context, NULL);
    struct cam_isp_stop_args isp = {.hw_stop_cmd=CAM_ISP_HW_STOP_IMMEDIATELY, .stop_only=true};
    struct cam_hw_stop_args stop = {&f->hw, &isp};
    return cam_ife_mgr_stop_hw(&g_ife_hw_mgr, &stop);
}
static void cleanup_checks(struct fixture *f, int mode, int active)
{
    if (mode < 2) {
        expect(signals == 3 && notifications == 1 && unlinks == (mode == 1));
        expect(list_empty(&f->context.pending_req_list) && list_empty(&f->context.wait_req_list) &&
               list_empty(&f->context.active_req_list));
        unsigned freed = 0;
        for (struct list_head *p = f->context.free_req_list.next; p != &f->context.free_req_list; p = p->next) ++freed;
        expect(freed == 3 && f->isp.substate_activated == CAM_ISP_CTX_ACTIVATED_HALT && !f->isp.active_req_cnt);
        expect(g_ife_hw_mgr.active_ctx_cnt == active - 1);
        if (mode == 0) expect(!f->isp.init_received && f->context.state == CAM_CTX_ACQUIRED);
    } else expect(g_ife_hw_mgr.active_ctx_cnt == active && !signals && !notifications);
}
#define CAM_IFE_PIX_PATH_RES_RDI_0 0
#define CAM_IFE_PIX_PATH_RES_RDI_1 1
#define CAM_IFE_PIX_PATH_RES_RDI_2 2
#define CAM_IFE_PIX_PATH_RES_RDI_3 3
#undef CAM_IFE_PIX_PATH_RES_IPP
#define CAM_IFE_PIX_PATH_RES_IPP 4
#define CAM_IFE_PIX_PATH_RES_PPP 5
#define CAM_IFE_PIX_PATH_RES_UDI_0 6
#define CAM_IFE_PIX_PATH_RES_UDI_1 7
#define CAM_IFE_PIX_PATH_RES_UDI_2 8
#undef CAM_IFE_PIX_PATH_RES_MAX
#define CAM_IFE_PIX_PATH_RES_MAX 9
#define CAM_ISP_RESOURCE_CID 2
#define CAM_ISP_IFE_IN_RES_TPG 9
#define CAM_ISP_RESOURCE_STATE_INIT_HW 2
#define spin_lock_irqsave(p, flags) do { (void)(p); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(p, flags) do { (void)(p); (void)(flags); } while (0)
struct cam_hw_info { void *core_info; };
struct cam_ife_csid_hw { struct cam_hw_intf *hw_intf; int lock_state, device_enabled, res_type; };
static int leaf_count, leaf_first, leaf_second;
static uint32_t leaf_mask, expected_mask;
static int leaf_result(bool poll)
{
    int at = leaf_count++;
    return at == leaf_first ? (poll ? -ETIMEDOUT : -EIO) : at == leaf_second ? -ENOSPC : 0;
}
static void cam_ife_csid_csi2_irq_ctrl(struct cam_ife_csid_hw *hw, bool on)
{ assert(hw && !on); }
static int cam_ife_csid_tpg_stop(struct cam_ife_csid_hw *hw, struct cam_isp_resource_node *res)
{ assert(hw && res); return leaf_result(false); }
static int cam_ife_csid_disable_pxl_path(struct cam_ife_csid_hw *hw, struct cam_isp_resource_node *res, unsigned cmd)
{ assert(hw && res && cmd == 1); return leaf_result(false); }
#define cam_ife_csid_disable_rdi_path cam_ife_csid_disable_pxl_path
#define cam_ife_csid_disable_udi_path cam_ife_csid_disable_pxl_path
static int cam_ife_csid_poll_stop_status(struct cam_ife_csid_hw *hw, uint32_t mask)
{ assert(hw); leaf_mask = mask; return leaf_result(true); }
#include "techpack/camera/drivers/cam_isp/isp_hw_mgr/isp_hw/ife_csid_hw/cam_ife_csid_core.c"

static void test_csid(void)
{
    struct cam_hw_intf intf = {0};
    struct cam_ife_csid_hw hw = {.hw_intf=&intf};
    struct cam_hw_info info = {.core_info=&hw};
    struct cam_isp_resource_node nodes[10], *pointers[10];
    struct cam_csid_hw_stop_args stop = {.num_res=10, .node_res=pointers, .stop_cmd=1};
    for (unsigned i = 0; i < 10; ++i) pointers[i] = &nodes[i];
    for (int tpg = 0; tpg < 2; ++tpg) {
        hw.res_type = tpg ? CAM_ISP_IFE_IN_RES_TPG : 0;
        int length = 10 + tpg;
        for (int first = -1; first < length; ++first)
        for (int second = -1; second < length; ++second) {
            if (second >= 0 && (first < 0 || second <= first)) continue;
            leaf_count = 0; leaf_mask = 0; expected_mask = (1U << 9) - 1;
            leaf_first = first; leaf_second = second; hw.device_enabled = 1;
            for (int i = 0; i < 10; ++i)
                nodes[i] = (struct cam_isp_resource_node){.res_id=i < 9 ? i : 0,
                    .res_type=i < 9 ? CAM_ISP_RESOURCE_PIX_PATH : CAM_ISP_RESOURCE_CID,
                    .res_state=CAM_ISP_RESOURCE_STATE_STREAMING};
            int want = first < 0 ? 0 : first == length - 1 ? -ETIMEDOUT : -EIO;
            expect(cam_ife_csid_stop(&info, &stop, sizeof(stop)) == want);
            expect(leaf_count == length && leaf_mask == expected_mask && !hw.device_enabled);
            for (unsigned i = 0; i < 10; ++i) expect(nodes[i].res_state == CAM_ISP_RESOURCE_STATE_INIT_HW);
            ++cases;
        }
    }
    hw.res_type = 0;
    for (unsigned invalid = 0; invalid < 2; ++invalid) {
        for (unsigned i = 0; i < 10; ++i)
            nodes[i] = (struct cam_isp_resource_node){.res_id=i < 9 ? (int)i : 0,
                .res_type=i < 9 ? CAM_ISP_RESOURCE_PIX_PATH : CAM_ISP_RESOURCE_CID,
                .res_state=CAM_ISP_RESOURCE_STATE_STREAMING};
        if (invalid) nodes[0].res_type = 999;
        else nodes[0].res_id = CAM_IFE_PIX_PATH_RES_MAX;
        leaf_count = 0; leaf_first = leaf_second = -1; leaf_mask = 0;
        expect(cam_ife_csid_stop(&info, &stop, sizeof(stop)) == -EINVAL);
        expect(leaf_count == 9 && leaf_mask == ((1U << 9) - 2));
        for (unsigned i = 0; i < 10; ++i) expect(nodes[i].res_state == CAM_ISP_RESOURCE_STATE_INIT_HW);
        ++cases;
    }
}

int main(void)
{
    struct fixture f;
    for (int mode = 0; mode < 3; ++mode)
    for (int active = 1; active <= 2; ++active) {
        fail_at = second_fail = -1; unlink_error = 0; setup(&f, active);
        expect(invoke(&f, mode) == 0); cleanup_checks(&f, mode, active); ++cases;
        int length = count; enum stage baseline[64]; memcpy(baseline, events, sizeof(events));
        setup(&f, active); f.ife[0].hw_ops.stop = NULL;
        expect(invoke(&f, mode) == -EINVAL); cleanup_checks(&f, mode, active); ++cases;
        for (int first = 0; first < length; ++first) {
            if (!injectable(baseline[first])) continue;
            for (int second = -1; second < length; ++second) {
                if (second >= 0 && (second <= first || !injectable(baseline[second]))) continue;
                for (int unlink = 0; unlink <= (mode == 1); ++unlink) {
                    fail_at = first; second_fail = second; unlink_error = unlink ? -ENOLINK : 0;
                    setup(&f, active);
                    int want = expected(baseline[first], false);
                    if (!want && second >= 0) want = expected(baseline[second], true);
                    if (!want) want = unlink_error;
                    expect(invoke(&f, mode) == want);
                    expect(count == length && !memcmp(events, baseline, length * sizeof(*events)));
                    expect(fault_calls == 1 + (second >= 0));
                    cleanup_checks(&f, mode, active); ++cases;
                }
            }
        }
    }
    test_csid();
    printf("PICO_CAMERA_STOP cases=%d defects=%d\n", cases, defects);
    return defects != 0;
}
