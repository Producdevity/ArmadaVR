/* SPDX-License-Identifier: MIT */
#include <xrt/xrt_space.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned live, failures;
static void *allocate(size_t size) { void *p = calloc(1, size); assert(p); live++; return p; }
static void release(void *p) { if (p) { assert(live); live--; free(p); } }
#define U_TYPED_CALLOC(type) ((type *)allocate(sizeof(type)))
#define free release
#define IPC_CHK_WITH_RET(connection, result, message, value) do { if (result != XRT_SUCCESS) return value; } while (0)
#define CHECK(condition) do { if (!(condition)) failures++; } while (0)

struct ipc_connection {
    uint32_t ids[6];
    unsigned destroyed[6];
    bool failed;
};
static xrt_result_t ipc_call_space_destroy(struct ipc_connection *c, uint32_t id)
{
    assert(id < 6);
    c->destroyed[id]++;
    return XRT_SUCCESS;
}
static xrt_result_t ipc_call_space_create_semantic_ids(struct ipc_connection *c,
    uint32_t *root, uint32_t *view, uint32_t *local, uint32_t *floor, uint32_t *stage, uint32_t *unbounded)
{
    if (c->failed) return XRT_ERROR_IPC_FAILURE;
    uint32_t *out[] = {root, view, local, floor, stage, unbounded};
    for (unsigned i = 0; i < 6; i++) *out[i] = c->ids[i];
    return XRT_SUCCESS;
}
#define UNUSED_OPERATION(name) static __typeof__(((struct xrt_space_overseer *)0)->name) name
UNUSED_OPERATION(create_offset_space);
UNUSED_OPERATION(create_pose_space);
UNUSED_OPERATION(locate_space);
UNUSED_OPERATION(locate_spaces);
UNUSED_OPERATION(locate_device);
UNUSED_OPERATION(ref_space_inc);
UNUSED_OPERATION(ref_space_dec);
UNUSED_OPERATION(recenter_local_spaces);
UNUSED_OPERATION(get_tracking_origin_offset);
UNUSED_OPERATION(set_tracking_origin_offset);
UNUSED_OPERATION(get_reference_space_offset);
UNUSED_OPERATION(set_reference_space_offset);
UNUSED_OPERATION(add_device);
UNUSED_OPERATION(remove_device);
UNUSED_OPERATION(attach_device);
#undef UNUSED_OPERATION
#include "space-under-test.h"

int main(void)
{
    struct ipc_connection failed = {.failed = true};
    CHECK(ipc_client_space_overseer_create(&failed) == NULL && live == 0);
    for (unsigned pattern = 0; pattern < 117649; pattern++) {
        struct ipc_connection c = {0};
        bool used[6] = {0};
        unsigned code = pattern;
        for (unsigned i = 0; i < 6; i++, code /= 7) {
            unsigned id = code % 7;
            c.ids[i] = id == 6 ? UINT32_MAX : id;
            if (id < 6) used[id] = true;
        }
        struct xrt_space_overseer *owner = ipc_client_space_overseer_create(&c);
        assert(owner);
        struct xrt_space *roles[] = {owner->semantic.root, owner->semantic.view, owner->semantic.local,
            owner->semantic.local_floor, owner->semantic.stage, owner->semantic.unbounded};
        struct xrt_space *borrowed = NULL;
        uint32_t borrowed_id = UINT32_MAX;
        for (unsigned i = 0; i < 6; i++) {
            CHECK((roles[i] == NULL) == (c.ids[i] == UINT32_MAX));
            if (!roles[i]) continue;
            CHECK(ipc_client_space(roles[i])->id == c.ids[i]);
            for (unsigned j = 0; j < i; j++)
                CHECK((roles[i] == roles[j]) == (c.ids[i] == c.ids[j]));
            if (!borrowed) { xrt_space_reference(&borrowed, roles[i]); borrowed_id = c.ids[i]; }
        }
        owner->destroy(owner);
        for (unsigned i = 0; i < 6; i++) CHECK(c.destroyed[i] == (used[i] && i != borrowed_id));
        xrt_space_reference(&borrowed, NULL);
        for (unsigned i = 0; i < 6; i++) CHECK(c.destroyed[i] == used[i]);
        CHECK(live == 0);
    }
    printf("MONADO_SPACE_CASES=117649 failures=%u live=%u\n", failures, live);
    return failures ? 1 : 0;
}
