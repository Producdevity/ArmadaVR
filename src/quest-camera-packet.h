#ifndef REFRAMED_QUEST_CAMERA_PACKET_H
#define REFRAMED_QUEST_CAMERA_PACKET_H
#include "quest-camera-isp.h"

#define QUEST_CAMERA_RDI_IO_SIZE 256

struct quest_camera_rdi_buffer {
    int32_t memory_handle, completion_fence;
    uint32_t width, height, stride, slice_height, bits, rdi;
    enum quest_camera_raw_format format;
    uint64_t allocation_size;
};

/* One nonsecure RDI0..3 output at offset zero. MIPI RAW8/RAW10 and PLAIN16_10
 * only; PLAIN8 and PLAIN16_8 have different/unsupported kernel RDI behavior.
 * Stride is in bytes and must be 16-byte aligned. Allocation covers every padded
 * row. Existing handles/fence must belong to the session. No I/O or ownership
 * change; failure preserves output. */
bool quest_camera_rdi_buffer_configure(const struct quest_camera_rdi_buffer *buffer,
                                     void *output, size_t size);

#define QUEST_CAMERA_PACKET_MAX_COMMANDS 8

enum quest_camera_packet_opcode {
    QUEST_CAMERA_PACKET_SENSOR_STREAM_ON = 0x01000000,
    QUEST_CAMERA_PACKET_SENSOR_UPDATE,
    QUEST_CAMERA_PACKET_SENSOR_INITIALIZE,
    QUEST_CAMERA_PACKET_SENSOR_PROBE,
    QUEST_CAMERA_PACKET_SENSOR_CONFIGURE,
    QUEST_CAMERA_PACKET_SENSOR_STREAM_OFF,
    QUEST_CAMERA_PACKET_SENSOR_NOP = 0x0100007f,
    QUEST_CAMERA_PACKET_PHY_CONFIGURE = 0x05000000,
    QUEST_CAMERA_PACKET_IFE_INITIALIZE = 0x0f000000,
    QUEST_CAMERA_PACKET_IFE_UPDATE,
};

struct quest_camera_command_buffer {
    int32_t memory_handle;
    uint32_t offset, capacity, length, type, metadata;
    uint64_t allocation_size;
};

#define QUEST_CAMERA_STREAM_PACKET_SIZE 336
/* Reusable streaming template: request ID and completion fence are zero; GET
 * transfers image ownership. MIPI RAW8/RAW10 only, with packed byte width in
 * the IO plane, matching the stock streaming path. Image width is supplied in
 * pixels; height includes the embedded metadata row. Pattern is explicit (0..7).
 * The command is an empty type0/meta0 buffer at offset zero, wholly available
 * for kernel writes. Allocations must remain alive until independent quiescence.
 * This does not configure or start streaming. Failure preserves output. */
size_t quest_camera_stream_packet_encode(const struct quest_camera_command_buffer *command,
    const struct quest_camera_rdi_buffer *image, uint32_t pattern, void *output, size_t size);

struct quest_camera_packet_config {
    enum quest_camera_packet_opcode opcode;
    uint64_t request_id;
    const struct quest_camera_command_buffer *commands;
    size_t command_count;
    int32_t kernel_command_index;
    uint32_t kernel_command_offset;
    const struct quest_camera_rdi_buffer *image;
};

/* Up to eight nonoverlapping command ranges and one optional RDI image.
 * IFE supports common direct (type5/meta3), CSID direct (5/18), and generic
 * (9/12) commands. Its kernel-write tail follows the used bytes of a common
 * direct command. Sensor commands use I2C (7/0), or LEGACY (10/0) for the first
 * probe command; PHY uses one direct (5/0) command. Payload content, operating
 * points and handle ownership are the caller's responsibility.
 * Sensor NOP has no commands/image and a positive request ID; its 72-byte size
 * exceeds the vendor sensor parser's strict sizeof(cam_packet) allocation check.
 * Keep all referenced allocations alive until completion or proven quiescence.
 * No patch lists, I/O or ownership changes. Failure preserves output. */
size_t quest_camera_packet_size(const struct quest_camera_packet_config *config);
size_t quest_camera_packet_encode(const struct quest_camera_packet_config *config,
                                 void *output, size_t size);

#endif
