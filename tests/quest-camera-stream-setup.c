#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t __s32;
#define CAM_PACKET_MAX_PLANES 3
#define CAM_MAX_STREAM_MODE_HANDLES 30
#define CAM_ISP_CTX_CFG_MAX 1
#define CAM_ISP_CTX_RES_MAX 1
#define GFP_KERNEL 0
#define __GFP_ZERO 0
#define IS_ERR_OR_NULL(p) (!(p))
#define CAM_ISP 0
#define CAM_UTIL 0
#define CAM_HW_MGR_CMD_INTERNAL 1
#define CAM_ISP_HW_MGR_GET_PACKET_OPCODE 2
#define CAM_ISP_HW_MGR_UPDATE_PATH_MASK 3
struct list_head { struct list_head *next, *prev; };
static void init_list(struct list_head *p) { p->next=p->prev=p; }
#define INIT_LIST_HEAD(p) init_list(p)
static bool list_empty(const struct list_head *p) { return p->next==p && p->prev==p; }
static void list_add_tail(struct list_head *p,struct list_head *h)
{ p->next=h;p->prev=h->prev;h->prev->next=p;h->prev=p; }
void list_del_init(struct list_head *p)
{ p->prev->next=p->next;p->next->prev=p->prev;init_list(p); }
static void log_message(int tag,const char *fmt,...) { (void)tag;(void)fmt; }
#define CAM_DBG(...) log_message(__VA_ARGS__)
#define CAM_INFO(...) log_message(__VA_ARGS__)
#define CAM_ERR(...) log_message(__VA_ARGS__)
#include "api.inc"
#include "kmd.inc"
struct cam_hw_update_entry { unsigned value; };
struct cam_hw_fence_map_entry { unsigned value; };
struct cam_isp_prepare_hw_update_data { struct cam_kmd_buf_info kmd_cmd_buff_info; };
struct cam_hw_mgr_dump_pf_data { unsigned unused; };
#include "image.inc"
struct cam_hw_prepare_update_args {
    void *ctxt_to_hw_map;
    uint32_t max_hw_update_entries,max_out_map_entries,max_in_map_entries;
    struct cam_hw_update_entry *hw_update_entries;
    struct cam_hw_fence_map_entry *out_map_entries,*in_map_entries;
    void *priv,*pf_data;
    struct cam_packet *packet;
    size_t remain_len;
    uint32_t req_stream_mask,num_hw_update_entries,num_out_map_entries,num_in_map_entries;
};
struct cam_hw_cmd_args { void *ctxt_to_hw_map; int cmd_type; union { void *internal_args; } u; };
struct cam_isp_hw_cmd_args {
    int cmd_type; void *cmd_data;
    union { unsigned packet_op_code; struct { unsigned path_irq_mask; } path_mask; } u;
};
struct hw_interface { void *hw_mgr_priv; int (*hw_cmd)(void *,void *); int (*hw_prepare_update)(void *,void *); };
struct cam_context { void *ctx_priv; unsigned ctx_id; struct hw_interface *hw_mgr_intf; };
struct cam_isp_context {
    void *hw_ctx;
    struct cam_isp_stream_image *stream_images,*stream_image_applied;
    unsigned num_stream_images;
    struct list_head stream_image_free_list,stream_image_ready_list,stream_image_umd_list,stream_image_active_list;
    int stream_image_completion;
    bool stream_image_wait;
    const void *substate_machine_irq;
};
static const int cam_isp_ctx_rdi_stream_activated_state_machine_irq[1]={1};
static void reinit_completion(int *p) { *p=0; }
static unsigned fault_image,fault_stage,current_image,refs[131],ref_gets[131],ref_puts[131];
static unsigned cases,defects,cycles,allocations;
static bool fail_allocation;
static uint64_t packet_memory[30][512],command_memory[30][128];
static void *kvmalloc_array(size_t count,size_t size,unsigned flags)
{ (void)flags;if(fail_allocation)return NULL;void *p=calloc(count,size);assert(p);allocations++;return p; }
static void kvfree(void *p) { if(p){assert(allocations);allocations--;free(p);} }
#define CHECK(v) do { cases++;if(!(v))defects++; } while(0)
static bool fail(unsigned stage) { return fault_stage==stage && current_image==fault_image; }
static int cam_mem_get_cpu_buf(int32_t handle,uintptr_t *address,size_t *length)
{
    assert((handle>=1 && handle<=30)||(handle>=101 && handle<=130));
    if(handle<=30) {
        current_image=(unsigned)handle-1;
        if(fail(1))return -ENOENT;
        *address=(uintptr_t)packet_memory[current_image];
        *length=fail(2)?sizeof(struct cam_packet)-1:sizeof(packet_memory[0]);
    } else {
        *address=(uintptr_t)command_memory[handle-101];
        *length=sizeof(command_memory[0]);
    }
    refs[handle]++;ref_gets[handle]++;return 0;
}
static void cam_mem_put_cpu_buf(int32_t handle)
{ assert(handle>0 && handle<=130 && refs[handle]);refs[handle]--;ref_puts[handle]++; }
#include "packet-functions.inc"
static int hw_cmd(void *owner,void *data)
{
    assert(owner==(void *)(uintptr_t)1);
    struct cam_hw_cmd_args *args=data;
    struct cam_isp_hw_cmd_args *isp=args->u.internal_args;
    assert(args->cmd_type==CAM_HW_MGR_CMD_INTERNAL);
    if(isp->cmd_type==CAM_ISP_HW_MGR_GET_PACKET_OPCODE) {
        if(fail(4))return -EIO;
        isp->u.packet_op_code=1;
    } else {
        assert(isp->cmd_type==CAM_ISP_HW_MGR_UPDATE_PATH_MASK);
        if(fail(5))return -ENOSPC;
        isp->u.path_mask.path_irq_mask=8;
    }
    return 0;
}
static int prepare(void *owner,void *data)
{
    assert(owner==(void *)(uintptr_t)1);
    struct cam_hw_prepare_update_args *cfg=data;
    struct cam_isp_prepare_hw_update_data *priv=cfg->priv;
    assert(cfg->max_hw_update_entries==1 && cfg->max_out_map_entries==1);
    assert(!cfg->max_in_map_entries && !cfg->in_map_entries && cfg->req_stream_mask==8);
    if(fail(6))return -ENOMEM;
    int rc=cam_packet_util_get_kmd_buffer(cfg->packet,&priv->kmd_cmd_buff_info);
    if(rc)return rc;
    if(fail(7))return -EIO;
    cfg->num_hw_update_entries=1;cfg->num_out_map_entries=1;
    cfg->num_in_map_entries=fail(8)?1:0;
    cfg->hw_update_entries[0].value=0x1234;
    cfg->out_map_entries[0].value=0x5678;
    return 0;
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "functions.inc"
#pragma GCC diagnostic pop
static void initialize(struct cam_isp_context *isp,struct cam_context *ctx,struct cam_set_stream_mode *cmd,unsigned count,bool shared)
{
    static struct hw_interface hw={.hw_mgr_priv=(void *)(uintptr_t)1,.hw_cmd=hw_cmd,.hw_prepare_update=prepare};
    memset(isp,0,sizeof(*isp));memset(ctx,0,sizeof(*ctx));memset(cmd,0,sizeof(*cmd));
    memset(refs,0,sizeof(refs));memset(ref_gets,0,sizeof(ref_gets));memset(ref_puts,0,sizeof(ref_puts));
    memset(packet_memory,0,sizeof(packet_memory));
    ctx->ctx_priv=isp;ctx->hw_mgr_intf=&hw;isp->hw_ctx=(void *)(uintptr_t)2;
    init_list(&isp->stream_image_free_list);init_list(&isp->stream_image_ready_list);
    init_list(&isp->stream_image_umd_list);init_list(&isp->stream_image_active_list);
    cmd->num_images=count;
    for(unsigned i=0;i<count && i<30;i++) {
        cmd->stream_images[i].packet_handle=i+1;cmd->stream_images[i].image_id=i+10;
        cmd->stream_images[i].mem_handles[0]=i+1000;
        struct cam_packet *packet=(void *)packet_memory[i];
        packet->num_cmd_buf=1;
        struct cam_cmd_buf_desc *desc=(void *)packet->payload;
        desc->mem_handle=101+(shared?0:i);desc->size=sizeof(command_memory[0]);
    }
}
static unsigned references(unsigned start,unsigned end)
{ unsigned n=0;for(unsigned i=start;i<=end;i++)n+=refs[i];return n; }
static void verify_success(struct cam_context *ctx,struct cam_set_stream_mode *cmd)
{
    struct cam_isp_context *isp=ctx->ctx_priv;
    CHECK(__cam_isp_ctx_set_stream_mode_in_acquired(ctx,cmd)==0);
    CHECK(isp->num_stream_images==cmd->num_images);
    CHECK(isp->substate_machine_irq==cam_isp_ctx_rdi_stream_activated_state_machine_irq);
    struct list_head *node=isp->stream_image_free_list.next;
    for(unsigned i=0;i<cmd->num_images;i++) {
        struct cam_isp_stream_image *image=&isp->stream_images[i];
        CHECK(node==&image->list_entry);node=node->next;
        CHECK(image->packet_handle==(int)i+1 && image->image_id==i+10);
        CHECK(image->mem_handles[0]==(int)i+1000 && !image->mem_handles[1] && !image->mem_handles[2]);
        CHECK(image->num_cfg==1 && image->num_fence_map_out==1);
        CHECK(image->cfg[0].value==0x1234 && image->fence_map_out[0].value==0x5678);
    }
    CHECK(node==&isp->stream_image_free_list);
    CHECK(references(1,30)==cmd->num_images && references(101,130)==cmd->num_images);
    void *old=isp->stream_images;
    CHECK(__cam_isp_ctx_set_stream_mode_in_acquired(ctx,cmd)==-EINVAL && old==isp->stream_images);
    release_excerpt(isp);
    CHECK(!references(1,30));CHECK(!references(101,130));
    CHECK(!isp->stream_images && !isp->num_stream_images && !allocations);
    CHECK(list_empty(&isp->stream_image_free_list));
}
int main(void)
{
    for(unsigned shared=0;shared<2;shared++)for(unsigned count=1;count<=30;count++) {
        for(unsigned stage=1;stage<=9;stage++)for(unsigned index=0;index<count;index++) {
            struct cam_context ctx;struct cam_isp_context isp;struct cam_set_stream_mode cmd;
            initialize(&isp,&ctx,&cmd,count,shared);fault_image=index;fault_stage=stage;
            if(stage==3)cmd.stream_images[index].packet_offset=sizeof(packet_memory[0])-sizeof(struct cam_packet);
            if(stage==9)((struct cam_cmd_buf_desc *)((struct cam_packet *)packet_memory[index])->payload)->size=sizeof(command_memory[0])+1;
            int rc=__cam_isp_ctx_set_stream_mode_in_acquired(&ctx,&cmd);
            CHECK(rc==(stage==4?-EIO:stage==5?-ENOSPC:stage==6||stage==7||stage==9?-EFAULT:-EINVAL));
            CHECK(!isp.stream_images && !isp.num_stream_images && !allocations);
            CHECK(!isp.substate_machine_irq);
            CHECK(list_empty(&isp.stream_image_free_list));
            CHECK(!references(1,30));CHECK(!references(101,130));
            /* The unpatched list can point into freed memory. Never traverse it. */
            initialize(&isp,&ctx,&cmd,count,shared);fault_stage=0;
            verify_success(&ctx,&cmd);cycles++;
        }
        struct cam_context ctx;struct cam_isp_context isp;struct cam_set_stream_mode cmd;
        initialize(&isp,&ctx,&cmd,count,shared);fault_stage=0;fail_allocation=true;
        CHECK(__cam_isp_ctx_set_stream_mode_in_acquired(&ctx,&cmd)==-EINVAL);
        CHECK(!isp.stream_images && list_empty(&isp.stream_image_free_list) && !references(1,130));
        fail_allocation=false;verify_success(&ctx,&cmd);cycles++;
    }
    for(unsigned count=0;count<=31;count+=31) {
        struct cam_context ctx;struct cam_isp_context isp;struct cam_set_stream_mode cmd;
        initialize(&isp,&ctx,&cmd,count,false);
        CHECK(__cam_isp_ctx_set_stream_mode_in_acquired(&ctx,&cmd)==-EINVAL);
        CHECK(!isp.stream_images && !allocations && !references(1,130));
    }
    printf("QUEST_CAMERA_STREAM_SETUP cycles=%u cases=%u defects=%u\n",cycles,cases,defects);
    return defects?1:0;
}
