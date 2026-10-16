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
#endif
