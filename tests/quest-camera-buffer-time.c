#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

struct list_head { struct list_head *next, *prev; };
static void init_list(struct list_head *p){p->next=p->prev=p;}
static bool list_empty(const struct list_head *p){return p->next==p;}
static void list_add_tail(struct list_head *p,struct list_head *head)
{p->next=head;p->prev=head->prev;head->prev->next=p;head->prev=p;}
static void list_del_init(struct list_head *p){p->prev->next=p->next;p->next->prev=p->prev;init_list(p);}
#define list_first_entry(head,type,member) ((type *)((char *)(head)->next - offsetof(type,member)))
#define list_for_each_entry(pos,head,member) \
    for(struct list_head *node=(head)->next;node!=(head) && ((pos)=(void *)((char *)node - offsetof(__typeof__(*(pos)),member)),true);node=node->next)
#define CAM_PACKET_MAX_PLANES 3
#define CAM_ISP_CTX_CFG_MAX 1
#define CAM_ISP_CTX_RES_MAX 1
#define CAM_MAX_STREAM_MODE_HANDLES 30
struct cam_hw_update_entry { unsigned unused; };
struct cam_hw_fence_map_entry { unsigned unused; };
struct cam_isp_prepare_hw_update_data { unsigned unused; };
struct cam_hw_mgr_dump_pf_data { unsigned unused; };
#include "image.inc"
#include "api.inc"
struct cam_context { void *ctx_priv; int ctx_mutex; unsigned ctx_id; };
struct cam_isp_context {
    struct cam_context *base;
    struct list_head stream_image_active_list,stream_image_ready_list,stream_image_free_list,stream_image_umd_list;
    struct cam_isp_stream_image *stream_image_applied;
    uint64_t sof_timestamp_val,monotonic_timestamp;
    int active_req_cnt,isp_mutex,stream_image_completion;
    bool stream_image_wait;
    int substate_activated;
};
#define CAM_ISP 0
#define CAM_ISP_CTX_ACTIVATED_HALT 99
static void test_log(int unused,const char *fmt,...){(void)unused;(void)fmt;}
#define CAM_DBG(...) test_log(__VA_ARGS__)
#define CAM_WARN(...) test_log(__VA_ARGS__)
#define CAM_ERR(...) test_log(__VA_ARGS__)
#define CAM_WARN_RATE_LIMIT(...) test_log(__VA_ARGS__)
#define CAM_ERR_RATE_LIMIT(...) test_log(__VA_ARGS__)
static void mutex_lock(int *p){assert(!*p);*p=1;}
static void mutex_unlock(int *p){assert(*p==1);*p=0;}
static void init_completion(int *p){*p=0;}
static void complete(int *p){*p=1;}
static long msecs_to_jiffies(uint32_t ms){return ms;}
static long wait_for_completion_timeout(int *p,long timeout){(void)timeout;return *p;}
static void __cam_isp_ctx_stream_report_cmd_get_state(struct cam_context *c,struct cam_isp_context *s,int rc)
{(void)c;(void)s;(void)rc;}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "functions.inc"
#pragma GCC diagnostic pop
static unsigned cases,defects;
#define CHECK(v) do{cases++;if(!(v))defects++;}while(0)
static void initialize(struct cam_isp_context *s,struct cam_context *c)
{
    memset(s,0,sizeof(*s));memset(c,0,sizeof(*c));s->base=c;c->ctx_priv=s;c->ctx_mutex=1;
    init_list(&s->stream_image_active_list);init_list(&s->stream_image_ready_list);
    init_list(&s->stream_image_free_list);init_list(&s->stream_image_umd_list);
}
static void arrived(struct cam_isp_context *s,struct cam_isp_stream_image *image,uint64_t tick,uint64_t mono)
{
    s->sof_timestamp_val=tick;s->monotonic_timestamp=mono;
    list_add_tail(&image->list_entry,&s->stream_image_active_list);s->active_req_cnt++;
    CHECK(__cam_isp_ctx_rdi_stream_buf_done_in_activated_state(s,NULL)==0);
    CHECK(!s->active_req_cnt && list_empty(&s->stream_image_active_list));
}
static void get(struct cam_context *c,uint64_t id,uint64_t tick,uint64_t mono,int64_t number)
{
    struct cam_set_stream_mode_cmd_get data={0};
    CHECK(__cam_isp_ctx_stream_mode_cmd_get_image(c,&data)==0);
    CHECK(data.num_images==1 && data.images[0].image_id==id);
    CHECK(data.images[0].timestamp==tick);
    CHECK(data.images[0].sof_timestamp==mono);
    CHECK(data.images[0].frame_num==number);
    CHECK(c->ctx_mutex==1 && !((struct cam_isp_context *)c->ctx_priv)->isp_mutex);
}
int main(void)
{
    for(unsigned cycle=0;cycle<1000;cycle++) {
        struct cam_context c;struct cam_isp_context s;initialize(&s,&c);
        struct cam_isp_stream_image images[2]={0};
        images[0].image_id=11;images[0].frame_num=100+cycle;
        images[1].image_id=12;images[1].frame_num=101+cycle;
        uint64_t tick=1000000000ULL+cycle*100000,mono=tick+5000000000ULL;
        arrived(&s,&images[0],tick,mono);arrived(&s,&images[1],tick+10000,mono+10000);
        CHECK(s.stream_image_completion && !list_empty(&s.stream_image_ready_list));
        s.monotonic_timestamp=mono+900000;s.sof_timestamp_val=tick+900000;
        get(&c,11,tick,mono,100+cycle);
        CHECK(s.stream_image_completion && !list_empty(&s.stream_image_ready_list));
        s.monotonic_timestamp=mono+1800000;
        get(&c,12,tick+10000,mono+10000,101+cycle);
        CHECK(!s.stream_image_completion && list_empty(&s.stream_image_ready_list));
        struct cam_set_stream_mode_cmd_return returned={.num_images=1,.image_ids={11}};
        CHECK(__cam_isp_ctx_stream_mode_cmd_ret_image(&c,&returned)==0);
        CHECK(list_first_entry(&s.stream_image_free_list,struct cam_isp_stream_image,list_entry)==&images[0]);
        list_del_init(&images[0].list_entry);images[0].frame_num=102+cycle;
        arrived(&s,&images[0],tick+20000,mono+20000);
        s.monotonic_timestamp=mono+2700000;
        get(&c,11,tick+20000,mono+20000,102+cycle);
        CHECK(list_empty(&s.stream_image_ready_list) && !s.stream_image_completion);
        struct cam_set_stream_mode_cmd_get empty={.num_images=29};
        CHECK(__cam_isp_ctx_stream_mode_cmd_get_image(&c,&empty)==-ENOENT && empty.num_images==0);
        s.stream_image_wait=true;empty.num_images=29;
        CHECK(__cam_isp_ctx_stream_mode_cmd_get_image(&c,&empty)==-EINVAL && empty.num_images==0);
        s.stream_image_wait=false;
        returned.image_ids[0]=11;CHECK(__cam_isp_ctx_stream_mode_cmd_ret_image(&c,&returned)==0);
        CHECK(__cam_isp_ctx_stream_mode_cmd_get_image(&c,&empty)==-ETIMEDOUT && !s.stream_image_wait && c.ctx_mutex==1 && !s.isp_mutex);
        CHECK(__cam_isp_ctx_rdi_stream_buf_done_in_activated_state(&s,NULL)==-EINVAL);
    }
    printf("QUEST_CAMERA_BUFFER_TIME cycles=1000 cases=%u defects=%u\n",cases,defects);
    return defects?1:0;
}
