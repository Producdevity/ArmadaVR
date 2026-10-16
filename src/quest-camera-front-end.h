#ifndef REFRAMED_QUEST_CAMERA_FRONT_END_H
#define REFRAMED_QUEST_CAMERA_FRONT_END_H
#include "quest-camera-allocation.h"
#include "quest-camera-control.h"
#include "quest-camera-ife.h"
#include "quest-camera-isp.h"
#include "quest-camera-blob.h"

struct quest_camera_front_end;
struct quest_camera_front_end_target { int crm_fd, device_fd; int32_t session, device; };
struct quest_camera_ife_setup {
    struct quest_camera_front_end_target target;
    struct quest_camera_isp_hardware hardware;
    struct quest_camera_ife_rdi input;
    int32_t command_mmu;
    enum quest_camera_memory_mapping command_mapping;
    struct quest_camera_clock_votes clocks;
    struct quest_camera_bandwidth_votes bandwidth;
    uint64_t csid_clock_hz;
};
enum quest_camera_front_end_buffer {
    QUEST_CAMERA_FRONT_END_DIRECT, QUEST_CAMERA_FRONT_END_GENERIC, QUEST_CAMERA_FRONT_END_PACKET,
    QUEST_CAMERA_FRONT_END_BUFFERS,
};
/* Own commands for one acquired Titan636 Lite with one local RDI0..3 path, or
 * one acquired PHY. Borrow descriptors/handles exclusively. Caller verifies
 * revision, MMU, routing, timing, rates and exclusive common-bus ownership.
 * No acquisition, scheduling, MCU transaction or stream-start is performed.
 * Inputs are copied before returning. Retain any nonnull owner after failure. */
int quest_camera_front_end_prepare_ife(const struct quest_camera_ife_setup *config,
                                      struct quest_camera_front_end **setup);
int quest_camera_front_end_prepare_phy(const struct quest_camera_front_end_target *target,
    const struct quest_camera_phy_config *config, struct quest_camera_front_end **setup);
/* One configuration attempt; keep all referenced memory after any result. */
int quest_camera_front_end_submit(struct quest_camera_front_end *setup);
/* Requires returned control calls and independent device/DMA/CPU quiescence,
 * including after partial preparation. A successful ioctl is insufficient. */
int quest_camera_front_end_quiesced(struct quest_camera_front_end *setup);
/* One cleanup attempt, retaining unresolved resources without retry. */
int quest_camera_front_end_release(struct quest_camera_front_end **setup);
bool quest_camera_front_end_resources(const struct quest_camera_front_end *setup,
    enum quest_camera_front_end_buffer buffer, struct quest_camera_allocation_view *view);
/* Local disposal only after independent quiescence and external cleanup of
 * every owned mapping, fd and CRM resource. Does not perform I/O. */
void quest_camera_front_end_closed(struct quest_camera_front_end **setup);
#endif
