#ifndef REFRAMED_PICO_CAMERA_ISP_H
#define REFRAMED_PICO_CAMERA_ISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_CAMERA_IFE_ACQUIRE_SIZE 264
#define PICO_CAMERA_CSIPHY_SIZE 32
#define PICO_CAMERA_RDI_IO_SIZE 256

enum pico_camera_raw_format {
    PICO_CAMERA_RAW_PLAIN8,
    PICO_CAMERA_RAW_PLAIN16,
    PICO_CAMERA_RAW_MIPI,
};

struct pico_camera_ife_config {
    uint32_t phy, cphy, lanes, lane_map;
    uint32_t virtual_channel, data_type, bits;
    uint32_t width, height;
    enum pico_camera_raw_format output;
};

/* Encode one uncropped, nonsecure CSI input and one RDI0 output for the b1977
 * Spectra acquire ABI. All sensor/PHY settings must come from the selected
 * hardware configuration. This validates representation, not physical wiring.
 * No resource is acquired and no device is opened. Failure preserves output. */
bool pico_camera_ife_acquire(const struct pico_camera_ife_config *config,
                             void *output, size_t size);

struct pico_camera_csiphy_config {
    uint32_t cphy, combo, lanes, lane_map;
    uint32_t settle_count;
    uint64_t data_rate;
    uint32_t mipi_flags;
};

/* Encode the nonsecure b1977 PHY payload using an explicit per-lane data rate
 * and sensor settle register count. The kernel divides the wire settle value
 * by 200000000; its UAPI comment calling the value milliseconds is inaccurate.
 * Active lane nibbles must be distinct and valid for the selected PHY mode.
 * No hardware is configured. Failure preserves output. */
bool pico_camera_csiphy_configure(const struct pico_camera_csiphy_config *config,
                                  void *output, size_t size);

struct pico_camera_rdi_buffer {
    int32_t memory_handle, completion_fence;
    uint32_t width, height, stride, slice_height, bits;
    enum pico_camera_raw_format format;
    uint64_t allocation_size;
};

/* Encode one nonsecure RDI0 output at offset zero. Only MIPI RAW8/RAW10 and
 * PLAIN16 are supported; handles and fence must already belong to the session.
 * Stride must meet the VFE bus's 16-byte alignment; the declared allocation
 * must cover every padded row. This neither allocates
 * nor transfers ownership of the buffer and does not submit a request. */
bool pico_camera_rdi_buffer_configure(const struct pico_camera_rdi_buffer *buffer,
                                     void *output, size_t size);

#define PICO_CAMERA_PACKET_MAX_COMMANDS 8

enum pico_camera_packet_opcode {
    PICO_CAMERA_PACKET_SENSOR_STREAM_ON = 0x01000000,
    PICO_CAMERA_PACKET_SENSOR_UPDATE,
    PICO_CAMERA_PACKET_SENSOR_INITIALIZE,
    PICO_CAMERA_PACKET_SENSOR_PROBE,
    PICO_CAMERA_PACKET_SENSOR_CONFIGURE,
    PICO_CAMERA_PACKET_SENSOR_STREAM_OFF,
    PICO_CAMERA_PACKET_PHY_CONFIGURE = 0x05000000,
    PICO_CAMERA_PACKET_IFE_INITIALIZE = 0x0f000000,
    PICO_CAMERA_PACKET_IFE_UPDATE,
};

struct pico_camera_command_buffer {
    int32_t memory_handle;
    uint32_t offset, capacity, length, type, metadata;
    uint64_t allocation_size;
};

struct pico_camera_packet_config {
    enum pico_camera_packet_opcode opcode;
    uint64_t request_id;
    const struct pico_camera_command_buffer *commands;
    size_t command_count;
    int32_t kernel_command_index;
    uint32_t kernel_command_offset;
    const struct pico_camera_rdi_buffer *image;
};

/* Encode a packet with up to eight direct/I2C/generic command references and
 * one optional RDI image. IFE packets require a kernel-write region immediately
 * after the used bytes of one command buffer. Other packets use index -1.
 * Callers retain every referenced allocation until completion or quiescence.
 * Inputs/output must not overlap. No patch list, device I/O or ownership change. */
size_t pico_camera_packet_size(const struct pico_camera_packet_config *config);
size_t pico_camera_packet_encode(const struct pico_camera_packet_config *config,
                                 void *output, size_t size);

#endif
