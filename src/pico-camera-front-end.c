#include "pico-camera-front-end.h"
#include "pico-camera-buffer.h"
#include "pico-camera-ife.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { DIRECT, GENERIC, PACKET, BUFFERS };
struct pico_camera_front_end {
    struct pico_camera_front_end_target target;
    struct pico_camera_buffer *buffers[BUFFERS];
    unsigned in_flight;
    bool prepared, submitted, quiesced;
};
static bool target_valid(const struct pico_camera_front_end_target *t)
{ return t && t->crm_fd >= 0 && t->device_fd >= 0 && t->session > 0 && t->device > 0; }
static bool single_rdi0(const struct pico_camera_ife_hardware *h)
{
    if (h->valid != 1 || !h->id[0] || h->id[0] > 0x40 || (h->id[0] & (h->id[0] - 1)) ||
        h->paths[0][0] != 8 || h->paths[0][1] || h->paths[0][2]) return false;
    for (unsigned i = 1; i < 5; ++i)
        if (h->id[i] || h->paths[i][0] || h->paths[i][1] || h->paths[i][2]) return false;
    return true;
}
static int prepare(const struct pico_camera_front_end_target *target,
                   const void *direct, size_t direct_size, const void *generic, size_t generic_size,
                   enum pico_camera_memory_mapping mapping, int32_t mmu,
                   struct pico_camera_front_end **setup)
{
    struct pico_camera_front_end *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    *setup = s; s->target = *target;
    bool ife = generic_size != 0;
    struct pico_camera_command_buffer references[2] = {0};
    for (unsigned i = DIRECT; i < PACKET; ++i) {
        size_t size = i == DIRECT ? direct_size : generic_size;
        if (!size) continue;
        bool mapped = ife && i == DIRECT;
        struct pico_camera_buffer_config c = {.kind = PICO_CAMERA_BUFFER_COMMAND,
            .mapping = mapped ? mapping : PICO_CAMERA_MEMORY_CPU,
            .size = mapped ? 8192 : 4096, .cached = true,
            .mmus = {mapped ? mmu : 0}, .mmu_count = mapped ? 1 : 0};
        int result = pico_camera_buffer_create(target->crm_fd, -1, &c, &s->buffers[i]);
        if (result) return result;
        struct pico_camera_buffer_view view;
        if (!pico_camera_buffer_view(s->buffers[i], &view)) return -EINVAL;
        result = pico_camera_buffer_cpu_begin(s->buffers[i], ION_BUFFER_WRITE);
        if (result) return result;
        memset(view.data, 0, view.size);
        memcpy(view.data, i == DIRECT ? direct : generic, size);
        result = pico_camera_buffer_cpu_end(s->buffers[i]);
        if (result) return result;
        references[i] = (struct pico_camera_command_buffer){.memory_handle = view.memory_handle,
            .capacity = (uint32_t)view.size, .length = (uint32_t)size, .allocation_size = view.size,
            .type = i == GENERIC ? 9 : 5, .metadata = ife ? (i == GENERIC ? 12 : 3) : 0};
    }
    struct pico_camera_buffer_config c = {.kind = PICO_CAMERA_BUFFER_PACKET,
        .mapping = PICO_CAMERA_MEMORY_CPU, .size = 4096, .cached = true};
    int result = pico_camera_buffer_create(target->crm_fd, -1, &c, &s->buffers[PACKET]);
    if (result) return result;
    struct pico_camera_buffer_view view;
    if (!pico_camera_buffer_view(s->buffers[PACKET], &view)) return -EINVAL;
    struct pico_camera_packet_config packet = {
        .opcode = ife ? PICO_CAMERA_PACKET_IFE_INITIALIZE : PICO_CAMERA_PACKET_PHY_CONFIGURE,
        .commands = references, .command_count = ife ? 2 : 1,
        .kernel_command_index = ife ? 0 : -1, .kernel_command_offset = ife ? (uint32_t)direct_size : 0};
    result = pico_camera_buffer_cpu_begin(s->buffers[PACKET], ION_BUFFER_WRITE);
    if (result) return result;
    memset(view.data, 0, view.size);
    bool encoded = pico_camera_packet_encode(&packet, view.data, view.size) != 0;
    result = pico_camera_buffer_cpu_end(s->buffers[PACKET]);
    if (result || !encoded) return result ? result : -EINVAL;
    s->prepared = true;
    return 0;
}
int pico_camera_front_end_prepare_ife(const struct pico_camera_ife_setup *c,
                                     struct pico_camera_front_end **setup)
{
    if (!c || !setup || *setup || !target_valid(&c->target) || !single_rdi0(&c->hardware) ||
        c->command_mmu < 0 || c->clocks.dual ||
        (c->command_mapping != PICO_CAMERA_MEMORY_IO && c->command_mapping != PICO_CAMERA_MEMORY_SHARED))
        return -EINVAL;
    uint8_t direct[PICO_CAMERA_IFE_RDI_COMMAND_SIZE + PICO_CAMERA_IFE_THROTTLE_COMMAND_SIZE];
    size_t offset = c->configure_throttle ? PICO_CAMERA_IFE_THROTTLE_COMMAND_SIZE : 0;
    if (offset && !pico_camera_ife_throttle_command(c->utilization, direct, sizeof(direct))) return -EINVAL;
    struct pico_camera_ife_rdi rdi = {c->hardware.id[0] >= 4, 0, c->sample_pattern, 1};
    if (!pico_camera_ife_rdi_commands(&rdi, direct + offset, sizeof(direct) - offset)) return -EINVAL;
    uint8_t generic[PICO_CAMERA_HFR_BLOB_SIZE + PICO_CAMERA_CLOCK_BLOB_SIZE + PICO_CAMERA_BANDWIDTH_BLOB_SIZE];
    if (!pico_camera_ife_hfr_blob(1, false, generic, sizeof(generic)) ||
        !pico_camera_ife_clock_blob(&c->clocks, generic + PICO_CAMERA_HFR_BLOB_SIZE, PICO_CAMERA_CLOCK_BLOB_SIZE) ||
        !pico_camera_ife_bandwidth_blob(&c->bandwidth, generic + PICO_CAMERA_HFR_BLOB_SIZE + PICO_CAMERA_CLOCK_BLOB_SIZE,
                                      PICO_CAMERA_BANDWIDTH_BLOB_SIZE)) return -EINVAL;
    return prepare(&c->target, direct, offset + PICO_CAMERA_IFE_RDI_COMMAND_SIZE, generic, sizeof(generic),
                   c->command_mapping, c->command_mmu, setup);
}
int pico_camera_front_end_prepare_phy(const struct pico_camera_front_end_target *target,
                                     const struct pico_camera_csiphy_config *config,
                                     struct pico_camera_front_end **setup)
{
    if (!setup || *setup || !target_valid(target)) return -EINVAL;
    uint8_t command[PICO_CAMERA_CSIPHY_SIZE];
    if (!pico_camera_csiphy_configure(config, command, sizeof(command))) return -EINVAL;
    return prepare(target, command, sizeof(command), NULL, 0, PICO_CAMERA_MEMORY_CPU, -1, setup);
}
int pico_camera_front_end_submit(struct pico_camera_front_end *s)
{
    if (!s || !s->prepared || s->submitted) return -EINVAL;
    s->submitted = true;
    for (unsigned i = 0; i < BUFFERS; ++i) {
        if (!s->buffers[i]) continue;
        int result = pico_camera_buffer_submit(s->buffers[i]);
        if (result) return result;
        s->in_flight |= 1U << i;
    }
    struct pico_camera_buffer_view packet;
    if (!pico_camera_buffer_view(s->buffers[PACKET], &packet)) return -EINVAL;
    return pico_camera_device_configure(s->target.device_fd, s->target.session, s->target.device,
                                        packet.memory_handle, 0, packet.size);
}
int pico_camera_front_end_quiesced(struct pico_camera_front_end *s)
{
    if (!s || !s->submitted || s->quiesced) return -EINVAL;
    for (unsigned i = 0; i < BUFFERS; ++i) {
        if (!(s->in_flight & (1U << i))) continue;
        int result = pico_camera_buffer_complete(s->buffers[i]);
        if (result) return result;
        s->in_flight &= ~(1U << i);
    }
    s->quiesced = true;
    return 0;
}
int pico_camera_front_end_release(struct pico_camera_front_end **setup)
{
    if (!setup) return -EINVAL;
    struct pico_camera_front_end *s = *setup;
    if (!s) return 0;
    if (s->submitted && !s->quiesced) return -EBUSY;
    s->prepared = false;
    for (unsigned i = 0; i < BUFFERS; ++i) {
        int result = pico_camera_buffer_release(&s->buffers[i]);
        if (result) return result;
    }
    free(s); *setup = NULL;
    return 0;
}
