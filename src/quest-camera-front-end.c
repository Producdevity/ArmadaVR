#include "quest-camera-front-end.h"
#include "quest-camera-packet.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { DIRECT, GENERIC, PACKET, BUFFERS };
struct quest_camera_front_end {
    struct quest_camera_front_end_target target;
    struct quest_camera_allocation *buffers[BUFFERS];
    bool prepared, submitted, failed, quiesced, releasing;
    int release_error;
};
static bool target_valid(const struct quest_camera_front_end_target *t)
{ return t && t->crm_fd >= 0 && t->device_fd >= 0 && t->crm_fd != t->device_fd && t->session > 0 && t->device > 0; }
static bool single_lite(const struct quest_camera_isp_hardware *h, uint32_t rdi)
{
    if (rdi > 3 || h->valid != 1 || !h->id[0] || (h->id[0] & ~0xf00fcU) ||
        (h->id[0] & (h->id[0] - 1)) || h->paths[0][0] != (8U << rdi) || h->paths[0][1] || h->paths[0][2]) return false;
    for (unsigned i = 1; i < 5; ++i)
        if (h->id[i] || h->paths[i][0] || h->paths[i][1] || h->paths[i][2]) return false;
    return true;
}
static int prepare_buffers(struct quest_camera_front_end *s,
    const void *direct, size_t direct_size, const void *generic, size_t generic_size,
    enum quest_camera_memory_mapping mapping, int32_t mmu)
{
    bool ife = generic_size != 0;
    struct quest_camera_command_buffer references[2] = {0};
    for (unsigned i = DIRECT; i < PACKET; ++i) {
        size_t size = i == DIRECT ? direct_size : generic_size;
        if (!size) continue;
        bool mapped = ife && i == DIRECT;
        struct quest_camera_allocation_config c = {.kind = QUEST_CAMERA_ALLOCATION_COMMAND,
            .mapping = mapped ? mapping : QUEST_CAMERA_MEMORY_CPU, .size = mapped ? 8192 : 4096,
            .mmus = {mapped ? mmu : 0}, .mmu_count = mapped ? 1 : 0};
        int result = quest_camera_allocation_create(s->target.crm_fd, -1, &c, &s->buffers[i]);
        if (result) return result;
        struct quest_camera_allocation_view view;
        if (!quest_camera_allocation_view(s->buffers[i], &view)) return -EINVAL;
        result = quest_camera_allocation_cpu_begin(s->buffers[i], QUEST_CAMERA_CPU_WRITE);
        if (result) return result;
        memset(view.data, 0, view.size);
        memcpy(view.data, i == DIRECT ? direct : generic, size);
        result = quest_camera_allocation_cpu_end(s->buffers[i]);
        if (result) return result;
        references[i] = (struct quest_camera_command_buffer){.memory_handle = view.handle,
            .capacity = (uint32_t)view.size, .length = (uint32_t)size, .allocation_size = view.size,
            .type = i == GENERIC ? 9 : 5, .metadata = ife ? (i == GENERIC ? 12 : 3) : 0};
    }
    struct quest_camera_allocation_config c = {.kind = QUEST_CAMERA_ALLOCATION_PACKET,
        .mapping = QUEST_CAMERA_MEMORY_CPU, .size = 4096};
    int result = quest_camera_allocation_create(s->target.crm_fd, -1, &c, &s->buffers[PACKET]);
    if (result) return result;
    struct quest_camera_allocation_view view;
    if (!quest_camera_allocation_view(s->buffers[PACKET], &view)) return -EINVAL;
    struct quest_camera_packet_config packet = {
        .opcode = ife ? QUEST_CAMERA_PACKET_IFE_INITIALIZE : QUEST_CAMERA_PACKET_PHY_CONFIGURE,
        .commands = references, .command_count = ife ? 2 : 1, .kernel_command_index = ife ? 0 : -1,
        .kernel_command_offset = ife ? (uint32_t)direct_size : 0};
    result = quest_camera_allocation_cpu_begin(s->buffers[PACKET], QUEST_CAMERA_CPU_WRITE);
    if (result) return result;
    memset(view.data, 0, view.size);
    bool encoded = quest_camera_packet_encode(&packet, view.data, view.size) != 0;
    result = quest_camera_allocation_cpu_end(s->buffers[PACKET]);
    return result ? result : encoded ? 0 : -EINVAL;
}
static int prepare(const struct quest_camera_front_end_target *t,
    const void *direct, size_t direct_size, const void *generic, size_t generic_size,
    enum quest_camera_memory_mapping mapping, int32_t mmu, struct quest_camera_front_end **setup)
{
    struct quest_camera_front_end *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    *setup = s; s->target = *t;
    int result = prepare_buffers(s, direct, direct_size, generic, generic_size, mapping, mmu);
    s->failed = result != 0; s->prepared = !s->failed;
    return result;
}
int quest_camera_front_end_prepare_ife(const struct quest_camera_ife_setup *c,
                                      struct quest_camera_front_end **setup)
{
    if (!c || !setup || *setup || !target_valid(&c->target) || !single_lite(&c->hardware, c->input.rdi) ||
        c->clocks.rdi != c->input.rdi || c->bandwidth.rdi != c->input.rdi || c->command_mmu < 0 ||
        (c->command_mapping != QUEST_CAMERA_MEMORY_IO && c->command_mapping != QUEST_CAMERA_MEMORY_SHARED)) return -EINVAL;
    uint8_t direct[QUEST_CAMERA_IFE_COMMON_SIZE + QUEST_CAMERA_IFE_RDI_SIZE];
    if (!quest_camera_ife_common_command(c->input.revision, direct, sizeof(direct)) ||
        !quest_camera_ife_rdi_commands(&c->input, direct + QUEST_CAMERA_IFE_COMMON_SIZE, QUEST_CAMERA_IFE_RDI_SIZE)) return -EINVAL;
    uint8_t generic[QUEST_CAMERA_HFR_BLOB_SIZE + QUEST_CAMERA_CLOCK_BLOB_SIZE +
                    QUEST_CAMERA_BANDWIDTH_BLOB_SIZE + QUEST_CAMERA_CSID_CLOCK_BLOB_SIZE];
    size_t offset = QUEST_CAMERA_HFR_BLOB_SIZE;
    if (!quest_camera_hfr_blob(c->input.rdi, generic, sizeof(generic)) ||
        !quest_camera_clock_blob(&c->clocks, generic + offset, QUEST_CAMERA_CLOCK_BLOB_SIZE)) return -EINVAL;
    offset += QUEST_CAMERA_CLOCK_BLOB_SIZE;
    if (!quest_camera_bandwidth_blob(&c->bandwidth, generic + offset, QUEST_CAMERA_BANDWIDTH_BLOB_SIZE)) return -EINVAL;
    offset += QUEST_CAMERA_BANDWIDTH_BLOB_SIZE;
    if (!quest_camera_csid_clock_blob(c->csid_clock_hz, generic + offset, QUEST_CAMERA_CSID_CLOCK_BLOB_SIZE)) return -EINVAL;
    return prepare(&c->target, direct, sizeof(direct), generic, sizeof(generic), c->command_mapping, c->command_mmu, setup);
}
int quest_camera_front_end_prepare_phy(const struct quest_camera_front_end_target *t,
    const struct quest_camera_phy_config *config, struct quest_camera_front_end **setup)
{
    if (!setup || *setup || !target_valid(t)) return -EINVAL;
    uint8_t direct[QUEST_CAMERA_PHY_CONFIG_SIZE];
    if (!quest_camera_phy_encode(config, direct, sizeof(direct))) return -EINVAL;
    return prepare(t, direct, sizeof(direct), NULL, 0, QUEST_CAMERA_MEMORY_CPU, -1, setup);
}
int quest_camera_front_end_submit(struct quest_camera_front_end *s)
{
    if (!s || !s->prepared || s->submitted || s->failed || s->quiesced || s->releasing) return -EINVAL;
    s->submitted = true;
    for (unsigned i = 0; i < BUFFERS; ++i) if (s->buffers[i]) {
        int result = quest_camera_allocation_submit(s->buffers[i]);
        if (result) return result;
    }
    struct quest_camera_allocation_view packet;
    if (!quest_camera_allocation_view(s->buffers[PACKET], &packet)) return -EINVAL;
    return quest_camera_device_configure(s->target.device_fd, s->target.session, s->target.device, packet.handle, 0, packet.size);
}
int quest_camera_front_end_quiesced(struct quest_camera_front_end *s)
{
    if (!s || s->quiesced || s->releasing) return -EINVAL;
    for (unsigned i = 0; i < BUFFERS; ++i) if (s->buffers[i]) {
        int result = quest_camera_allocation_quiesced(s->buffers[i]);
        if (result) return result;
    }
    s->quiesced = true; s->prepared = false;
    return 0;
}
int quest_camera_front_end_release(struct quest_camera_front_end **setup)
{
    if (!setup) return -EINVAL;
    struct quest_camera_front_end *s = *setup;
    if (!s) return 0;
    if ((s->submitted || s->failed) && !s->quiesced) return -EBUSY;
    if (s->releasing) return s->release_error;
    s->releasing = true; s->prepared = false;
    for (unsigned i = 0; i < BUFFERS; ++i) {
        int result = quest_camera_allocation_release(&s->buffers[i]);
        if (result && !s->release_error) s->release_error = result;
    }
    if (s->release_error) return s->release_error;
    free(s); *setup = NULL;
    return 0;
}
bool quest_camera_front_end_resources(const struct quest_camera_front_end *s,
    enum quest_camera_front_end_buffer buffer, struct quest_camera_allocation_view *view)
{
    if (!s || buffer < 0 || buffer >= QUEST_CAMERA_FRONT_END_BUFFERS) return false;
    return quest_camera_allocation_resources(s->buffers[buffer], view);
}
void quest_camera_front_end_closed(struct quest_camera_front_end **setup)
{
    if (!setup || !*setup) return;
    struct quest_camera_front_end *s = *setup;
    for (unsigned i = 0; i < BUFFERS; ++i) quest_camera_allocation_closed(&s->buffers[i]);
    free(s); *setup = NULL;
}
