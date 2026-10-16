#include "quest-camera-packet.h"
#include <string.h>

static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

bool quest_camera_rdi_buffer_configure(const struct quest_camera_rdi_buffer *b,
                                     void *output, size_t size)
{
    if (!b || !output || size < QUEST_CAMERA_RDI_IO_SIZE || b->memory_handle <= 0 ||
        b->completion_fence <= 0 || b->rdi > 3 || !b->width || b->width > 4096 ||
        !b->height || b->height > 4096 || b->slice_height < b->height || b->slice_height > 65536 || b->stride > 65536 ||
        (b->bits != 8 && b->bits != 10)) return false;
    uint32_t format, row_size;
    switch (b->format) {
    case QUEST_CAMERA_RAW_MIPI:
        if (b->bits == 10 && b->width % 4) return false;
        format = b->bits == 8 ? 2 : 3;
        row_size = (b->width * b->bits + 7) / 8;
        break;
    case QUEST_CAMERA_RAW_PLAIN16:
        if (b->bits != 10) return false;
        format = 14;
        row_size = b->width * 2;
        break;
    default: return false;
    }
    uint64_t required = (uint64_t)b->stride * b->slice_height;
    if (b->stride < row_size || b->stride % 16 || required > UINT32_MAX || required > b->allocation_size || b->allocation_size > UINT32_MAX) return false;
    uint8_t bytes[QUEST_CAMERA_RDI_IO_SIZE] = {0};
    put32(bytes, (uint32_t)b->memory_handle);
    put32(bytes + 24, b->width);
    put32(bytes + 28, b->height);
    put32(bytes + 32, b->stride);
    put32(bytes + 36, b->slice_height);
    put32(bytes + 168, format);
    put32(bytes + 176, 5);
    put32(bytes + 180, b->bits);
    put32(bytes + 188, 0x3006 + b->rdi);
    put32(bytes + 192, (uint32_t)b->completion_fence);
    put32(bytes + 224, 2);
    put32(bytes + 232, 1);
    put32(bytes + 240, 1);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}

size_t quest_camera_packet_size(const struct quest_camera_packet_config *c)
{
    if (!c || c->request_id > INT64_MAX) return 0;
    if (c->opcode == QUEST_CAMERA_PACKET_SENSOR_NOP)
        return c->request_id && !c->commands && !c->command_count && !c->image &&
            c->kernel_command_index == -1 && !c->kernel_command_offset ? 72 : 0;
    if (!c->commands || !c->command_count || c->command_count > QUEST_CAMERA_PACKET_MAX_COMMANDS) return 0;
    bool ife = false;
    switch (c->opcode) {
    case QUEST_CAMERA_PACKET_IFE_INITIALIZE:
        if (c->request_id || c->image) return 0;
        ife = true;
        break;
    case QUEST_CAMERA_PACKET_IFE_UPDATE:
        if (!c->request_id || !c->image) return 0;
        ife = true;
        break;
    case QUEST_CAMERA_PACKET_SENSOR_UPDATE:
        if (!c->request_id) return 0;
        break;
    case QUEST_CAMERA_PACKET_SENSOR_STREAM_ON:
    case QUEST_CAMERA_PACKET_SENSOR_INITIALIZE:
    case QUEST_CAMERA_PACKET_SENSOR_PROBE:
    case QUEST_CAMERA_PACKET_SENSOR_CONFIGURE:
    case QUEST_CAMERA_PACKET_SENSOR_STREAM_OFF:
    case QUEST_CAMERA_PACKET_PHY_CONFIGURE:
        if (c->request_id) return 0;
        break;
    default: return 0;
    }
    if (c->opcode == QUEST_CAMERA_PACKET_PHY_CONFIGURE && c->command_count != 1) return 0;
    if (!ife && (c->image || c->kernel_command_index != -1 || c->kernel_command_offset)) return 0;
    if (ife && (c->kernel_command_index < 0 || (size_t)c->kernel_command_index >= c->command_count)) return 0;
    for (size_t i = 0; i < c->command_count; ++i) {
        const struct quest_camera_command_buffer *b = c->commands + i;
        if (b->memory_handle <= 0 || !b->capacity || b->length > b->capacity ||
            ((b->offset | b->capacity | b->length) & 3) ||
            (uint64_t)b->offset + b->capacity > b->allocation_size ||
            b->allocation_size > UINT32_MAX ||
            (c->image && c->image->memory_handle == b->memory_handle)) return 0;
        if (ife) {
            if (!((b->type == 5 && (b->metadata == 3 || b->metadata == 18)) ||
                  (b->type == 9 && b->metadata == 12))) return 0;
        } else if (b->metadata ||
                   (c->opcode == QUEST_CAMERA_PACKET_PHY_CONFIGURE ? b->type != 5 :
                    b->type != 7 && !(c->opcode == QUEST_CAMERA_PACKET_SENSOR_PROBE && i == 0 && b->type == 10)))
            return 0;
        for (size_t j = 0; j < i; ++j) {
            const struct quest_camera_command_buffer *a = c->commands + j;
            if (a->memory_handle == b->memory_handle &&
                (a->allocation_size != b->allocation_size ||
                 (a->offset < (uint64_t)b->offset + b->capacity &&
                  b->offset < (uint64_t)a->offset + a->capacity))) return 0;
        }
        if (ife && (size_t)c->kernel_command_index == i) {
            if (b->type != 5 || b->metadata != 3 || c->kernel_command_offset != b->length || b->capacity - b->length < 4) return 0;
        } else if (!b->length) return 0;
    }
    if (c->image) {
        uint8_t image[QUEST_CAMERA_RDI_IO_SIZE];
        if (!quest_camera_rdi_buffer_configure(c->image, image, sizeof(image))) return 0;
    }
    return 56 + c->command_count * 24 + (c->image ? QUEST_CAMERA_RDI_IO_SIZE : 0);
}

size_t quest_camera_packet_encode(const struct quest_camera_packet_config *c,
                                 void *output, size_t size)
{
    size_t required = quest_camera_packet_size(c);
    if (!required || !output || size < required) return 0;
    uint8_t bytes[56 + QUEST_CAMERA_PACKET_MAX_COMMANDS * 24 + QUEST_CAMERA_RDI_IO_SIZE] = {0};
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
        const struct quest_camera_command_buffer *b = c->commands + i;
        uint8_t *descriptor = bytes + 56 + i * 24;
        put32(descriptor, (uint32_t)b->memory_handle);
        put32(descriptor + 4, b->offset);
        put32(descriptor + 8, b->capacity);
        put32(descriptor + 12, b->length);
        put32(descriptor + 16, b->type);
        put32(descriptor + 20, b->metadata);
    }
    if (c->image && !quest_camera_rdi_buffer_configure(c->image,
            bytes + 56 + c->command_count * 24, QUEST_CAMERA_RDI_IO_SIZE)) return 0;
    memcpy(output, bytes, required);
    return required;
}

