#ifndef REFRAMED_QUEST_CAMERA_MCU_H
#define REFRAMED_QUEST_CAMERA_MCU_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QUEST_CAMERA_METADATA_QUERY_SIZE 25
#define QUEST_CAMERA_METADATA_REPLY_SIZE 60
#define QUEST_CAMERA_METADATA_MAX_TYPES 5

struct quest_camera_mcu_parameters {
    uint32_t camera_bit;
    uint32_t words[3];
};

/* Query one representative camera per hardware type, in caller-supplied order.
 * IDs are distinct one-hot masks, count 1..5. Sequence 1..254 must already be
 * allocated by this control owner. Output is the complete Syncboss command-2
 * packet with subcommand 0x4e and the fixed 21-byte query body. No I/O.
 * The caller must match reply type 0x4e, sequence and MCU generation first.
 * Decode accepts the exact 60-byte payload; it does not validate active timing,
 * select cameras or imply that these parameters survived a mode change.
 * Failure preserves output. */
bool quest_camera_metadata_query_encode(const uint32_t *camera_bits, size_t count,
                                        uint8_t sequence, void *output, size_t size);
bool quest_camera_metadata_reply_decode(const uint32_t *camera_bits, size_t count,
                                        const void *payload, size_t size,
                                        struct quest_camera_mcu_parameters *output,
                                        size_t output_count);
enum quest_camera_mcu_operation {
    QUEST_CAMERA_MCU_START = 0x2c,
    QUEST_CAMERA_MCU_STOP = 0x2d,
    QUEST_CAMERA_MCU_INITIALIZE = 0x2e,
    QUEST_CAMERA_MCU_DEINITIALIZE = 0x2f,
};
#define QUEST_CAMERA_MCU_COMMAND_SIZE 7
#define QUEST_CAMERA_MCU_STATUS_TYPE 0x7d
#define QUEST_CAMERA_MCU_STATUS_SIZE 4

/* Encode the direct command type, allocated sequence and four-byte camera mask.
 * These are not subcommands of command 2. No hardware operation occurs here.
 * Initialization/settings, metadata reset, stream generation and exclusive MCU
 * ownership must be managed separately; encoding a start is not preparation.
 * The reply must already match type 0x7d, sequence and generation. Status zero
 * acknowledges the command; it does not prove DMA or device quiescence.
 * Failure preserves output. Zero camera masks are rejected as no-ops. */
bool quest_camera_mcu_command_encode(enum quest_camera_mcu_operation operation,
                                      uint32_t camera_mask, uint8_t sequence, void *output, size_t size);
bool quest_camera_mcu_status_decode(const void *payload, size_t size, int32_t *status);
enum quest_camera_mcu_setting {
    QUEST_CAMERA_MCU_BINNING = 0xa7,
    QUEST_CAMERA_MCU_BPP = 0xa0,
    QUEST_CAMERA_MCU_SYNC_MODE = 0xa4,
    QUEST_CAMERA_MCU_IMAGE = 0xa5,
    QUEST_CAMERA_MCU_MIPI_CLOCK = 0x66,
    QUEST_CAMERA_MCU_METADATA_MODE = 0xdd,
    QUEST_CAMERA_MCU_GAMMA = 0x31,
};
struct quest_camera_mcu_setting_record {
    uint32_t camera_mask;
    uint8_t value, second;
};
#define QUEST_CAMERA_MCU_SETTING_MAX_RECORDS 15
#define QUEST_CAMERA_MCU_SETTING_MAX_SIZE 94

/* Command 3 carries the setting subcommand and packed records. Masks must be
 * nonzero and disjoint. MIPI clock, metadata mode and gamma accept one record;
 * gamma accepts only 0 or 1. Only image configuration uses the second byte.
 * Other values are firmware selectors, not inferred physical rates or modes.
 * The caller supplies a verified operating point and owns the allocated sequence.
 * No I/O or local firmware-library event bookkeeping occurs. Reply type/status
 * follow the direct command contract above. Failure preserves output and written. */
bool quest_camera_mcu_setting_encode(enum quest_camera_mcu_setting setting,
    const struct quest_camera_mcu_setting_record *records, size_t count,
    uint8_t sequence, void *output, size_t capacity, size_t *written);
#endif
