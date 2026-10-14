#include "pico-camera-isp.h"

#include <string.h>

static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

bool pico_camera_ife_acquire(const struct pico_camera_ife_config *c, void *output, size_t size)
{
    if (!c || !output || size < PICO_CAMERA_IFE_ACQUIRE_SIZE || c->phy > 5 || c->cphy > 1 ||
        c->lanes == 0 || c->lanes > (c->cphy ? 3U : 4U) || c->lane_map > UINT16_MAX ||
        c->virtual_channel > 3 || (c->bits != 8 && c->bits != 10) ||
        c->data_type != (c->bits == 8 ? 0x2aU : 0x2bU) ||
        !c->width || c->width > 4096 || !c->height || c->height > 4096) return false;

    uint32_t input_format = c->bits == 8 ? 2 : 3;
    uint32_t output_format;
    switch (c->output) {
    case PICO_CAMERA_RAW_PLAIN8:
        if (c->bits != 8) return false;
        output_format = 12;
        break;
    case PICO_CAMERA_RAW_PLAIN16:
        output_format = c->bits == 8 ? 13 : 14;
        break;
    case PICO_CAMERA_RAW_MIPI:
        output_format = input_format;
        break;
    default: return false;
    }

    /* Stock includes an unused output-port-sized tail and the acquire header's
     * data member in its allocation. Keep those bytes zero and retain its size. */
    uint8_t bytes[PICO_CAMERA_IFE_ACQUIRE_SIZE] = {0};
    put32(bytes, 0x2000);
    put32(bytes + 8, 1);
    put32(bytes + 12, 0x200);
    put32(bytes + 16, 232);
    uint8_t *in = bytes + 24;
    put32(in, 0x4001 + c->phy);
    put32(in + 4, c->cphy);
    put32(in + 8, c->lanes);
    put32(in + 12, c->lane_map);
    put32(in + 16, c->virtual_channel);
    put32(in + 32, c->data_type);
    put32(in + 48, 1);
    put32(in + 52, input_format);
    put32(in + 68, c->width - 1);
    put32(in + 72, c->width);
    put32(in + 92, c->height - 1);
    put32(in + 96, c->height);
    put32(in + 120, 1);
    uint8_t *out = in + 152;
    put32(out, 0x3006);
    put32(out + 4, output_format);
    put32(out + 8, c->width);
    put32(out + 12, c->height);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}

bool pico_camera_csiphy_configure(const struct pico_camera_csiphy_config *c, void *output, size_t size)
{
    if (!c || !output || size < PICO_CAMERA_CSIPHY_SIZE || c->cphy > 1 || c->combo > 1 ||
        c->lanes == 0 || c->lanes > (c->cphy ? 3U : c->combo ? 2U : 4U) ||
        c->lane_map > UINT16_MAX || c->settle_count > UINT16_MAX || !c->data_rate) return false;

    uint32_t seen = 0, mask = 2;
    for (unsigned i = 0; i < c->lanes; ++i) {
        unsigned lane = (c->lane_map >> (i * 4)) & 15;
        if (lane >= (c->cphy || c->combo ? 3U : 4U) || (seen & (1U << lane))) return false;
        seen |= 1U << lane;
        mask |= i == 0 && lane == 0 ? 1U : 1U << (lane + 1);
    }
    if (c->combo) mask = 0x18;
    uint64_t settle = (uint64_t)c->settle_count * 200000000;
    uint8_t bytes[PICO_CAMERA_CSIPHY_SIZE] = {0};
    bytes[0] = (uint8_t)mask;
    bytes[1] = (uint8_t)(mask >> 8);
    bytes[2] = (uint8_t)c->lane_map;
    bytes[3] = (uint8_t)(c->lane_map >> 8);
    bytes[4] = (uint8_t)c->cphy;
    bytes[5] = (uint8_t)c->combo;
    bytes[6] = (uint8_t)c->lanes;
    for (unsigned i = 0; i < 8; ++i) {
        bytes[8 + i] = (uint8_t)(settle >> (8 * i));
        bytes[16 + i] = (uint8_t)(c->data_rate >> (8 * i));
    }
    put32(bytes + 24, c->mipi_flags);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}

bool pico_camera_rdi_buffer_configure(const struct pico_camera_rdi_buffer *b,
                                     void *output, size_t size)
{
    if (!b || !output || size < PICO_CAMERA_RDI_IO_SIZE || b->memory_handle <= 0 ||
        b->completion_fence <= 0 || !b->width || b->width > 4096 ||
        !b->height || b->height > 4096 || b->slice_height < b->height ||
        (b->bits != 8 && b->bits != 10)) return false;
    uint32_t format, row_size;
    switch (b->format) {
    case PICO_CAMERA_RAW_MIPI:
        format = b->bits == 8 ? 2 : 3;
        row_size = (b->width * b->bits + 7) / 8;
        break;
    case PICO_CAMERA_RAW_PLAIN16:
        format = b->bits == 8 ? 13 : 14;
        row_size = b->width * 2;
        break;
    default: return false;
    }
    uint64_t required = (uint64_t)b->stride * b->slice_height;
    if (b->stride < row_size || b->stride % 16 || required > UINT32_MAX || required > b->allocation_size) return false;
    uint8_t bytes[PICO_CAMERA_RDI_IO_SIZE] = {0};
    put32(bytes, (uint32_t)b->memory_handle);
    put32(bytes + 24, b->width);
    put32(bytes + 28, b->height);
    put32(bytes + 32, b->stride);
    put32(bytes + 36, b->slice_height);
    put32(bytes + 168, format);
    put32(bytes + 176, 5);
    put32(bytes + 180, b->bits);
    put32(bytes + 188, 0x3006);
    put32(bytes + 192, (uint32_t)b->completion_fence);
    put32(bytes + 224, 2);
    put32(bytes + 232, 1);
    put32(bytes + 240, 1);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}

size_t pico_camera_packet_size(const struct pico_camera_packet_config *c)
{
    if (!c || !c->commands || !c->command_count || c->command_count > PICO_CAMERA_PACKET_MAX_COMMANDS) return 0;
    bool ife = false;
    switch (c->opcode) {
    case PICO_CAMERA_PACKET_IFE_INITIALIZE:
        if (c->request_id || c->image) return 0;
        ife = true;
        break;
    case PICO_CAMERA_PACKET_IFE_UPDATE:
        if (!c->request_id || !c->image) return 0;
        ife = true;
        break;
    case PICO_CAMERA_PACKET_SENSOR_UPDATE:
        if (!c->request_id) return 0;
        break;
    case PICO_CAMERA_PACKET_SENSOR_STREAM_ON:
    case PICO_CAMERA_PACKET_SENSOR_INITIALIZE:
    case PICO_CAMERA_PACKET_SENSOR_PROBE:
    case PICO_CAMERA_PACKET_SENSOR_CONFIGURE:
    case PICO_CAMERA_PACKET_SENSOR_STREAM_OFF:
    case PICO_CAMERA_PACKET_PHY_CONFIGURE:
        if (c->request_id) return 0;
        break;
    default: return 0;
    }
    if (!ife && (c->image || c->kernel_command_index != -1 || c->kernel_command_offset)) return 0;
    if (ife && (c->kernel_command_index < 0 || (size_t)c->kernel_command_index >= c->command_count)) return 0;
    for (size_t i = 0; i < c->command_count; ++i) {
        const struct pico_camera_command_buffer *b = c->commands + i;
        if (b->memory_handle <= 0 || !b->capacity || b->length > b->capacity ||
            ((b->offset | b->capacity | b->length) & 3) ||
            (uint64_t)b->offset + b->capacity > b->allocation_size ||
            (uint64_t)b->offset + b->capacity > UINT32_MAX ||
            (b->type != 5 && b->type != 7 && b->type != 9) ||
            (c->image && c->image->memory_handle == b->memory_handle)) return 0;
        if (ife && (size_t)c->kernel_command_index == i) {
            if (b->type != 5 || c->kernel_command_offset != b->length || b->capacity - b->length < 4) return 0;
        } else if (!b->length) return 0;
    }
    if (ife) {
        const struct pico_camera_command_buffer *kernel = c->commands + c->kernel_command_index;
        uint64_t start = (uint64_t)kernel->offset + kernel->length;
        uint64_t end = (uint64_t)kernel->offset + kernel->capacity;
        for (size_t i = 0; i < c->command_count; ++i) {
            const struct pico_camera_command_buffer *b = c->commands + i;
            if ((size_t)c->kernel_command_index != i && b->memory_handle == kernel->memory_handle &&
                start < (uint64_t)b->offset + b->capacity && b->offset < end) return 0;
        }
    }
    if (c->image) {
        uint8_t image[PICO_CAMERA_RDI_IO_SIZE];
        if (!pico_camera_rdi_buffer_configure(c->image, image, sizeof(image))) return 0;
    }
    return 56 + c->command_count * 24 + (c->image ? PICO_CAMERA_RDI_IO_SIZE : 0);
}

size_t pico_camera_packet_encode(const struct pico_camera_packet_config *c,
                                 void *output, size_t size)
{
    size_t required = pico_camera_packet_size(c);
    if (!required || !output || size < required) return 0;
    uint8_t bytes[56 + PICO_CAMERA_PACKET_MAX_COMMANDS * 24 + PICO_CAMERA_RDI_IO_SIZE] = {0};
    put32(bytes, (uint32_t)c->opcode);
    put32(bytes + 4, (uint32_t)required);
    for (unsigned i = 0; i < 8; ++i) bytes[8 + i] = (uint8_t)(c->request_id >> (8 * i));
    put32(bytes + 28, (uint32_t)c->command_count);
    put32(bytes + 32, (uint32_t)c->command_count * 24);
    put32(bytes + 36, c->image ? 1 : 0);
    put32(bytes + 40, (uint32_t)required - 56);
    put32(bytes + 48, (uint32_t)c->kernel_command_index);
    put32(bytes + 52, c->kernel_command_offset);
    for (size_t i = 0; i < c->command_count; ++i) {
        const struct pico_camera_command_buffer *b = c->commands + i;
        uint8_t *descriptor = bytes + 56 + i * 24;
        put32(descriptor, (uint32_t)b->memory_handle);
        put32(descriptor + 4, b->offset);
        put32(descriptor + 8, b->capacity);
        put32(descriptor + 12, b->length);
        put32(descriptor + 16, b->type);
        put32(descriptor + 20, b->metadata);
    }
    if (c->image && !pico_camera_rdi_buffer_configure(c->image,
            bytes + 56 + c->command_count * 24, PICO_CAMERA_RDI_IO_SIZE)) return 0;
    memcpy(output, bytes, required);
    return required;
}
