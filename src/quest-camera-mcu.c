#include "quest-camera-mcu.h"
#include <string.h>

static bool valid_ids(const uint32_t *bits, size_t count)
{
    if (!bits || !count || count > QUEST_CAMERA_METADATA_MAX_TYPES) return false;
    uint32_t seen = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t bit = bits[i];
        if (!bit || (bit & (bit - 1)) || (seen & bit)) return false;
        seen |= bit;
    }
    return true;
}
bool quest_camera_metadata_query_encode(const uint32_t *camera_bits, size_t count,
                                        uint8_t sequence, void *output, size_t size)
{
    if (!valid_ids(camera_bits, count) || !sequence || sequence == 255 || !output ||
        size < QUEST_CAMERA_METADATA_QUERY_SIZE) return false;
    uint8_t bytes[QUEST_CAMERA_METADATA_QUERY_SIZE] = {2, sequence, 22, 0x4e, (uint8_t)count};
    for (size_t i = 0; i < count; ++i)
        for (unsigned j = 0; j < 4; ++j) bytes[5 + i * 4 + j] = (uint8_t)(camera_bits[i] >> (8 * j));
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
bool quest_camera_metadata_reply_decode(const uint32_t *camera_bits, size_t count,
                                        const void *payload, size_t size,
                                        struct quest_camera_mcu_parameters *output,
                                        size_t output_count)
{
    if (!valid_ids(camera_bits, count) || !payload || size != QUEST_CAMERA_METADATA_REPLY_SIZE ||
        !output || output_count != count) return false;
    struct quest_camera_mcu_parameters values[QUEST_CAMERA_METADATA_MAX_TYPES] = {0};
    const uint8_t *bytes = payload;
    for (size_t i = 0; i < count; ++i) {
        values[i].camera_bit = camera_bits[i];
        for (unsigned column = 0; column < 3; ++column)
            for (unsigned j = 0; j < 4; ++j)
                values[i].words[column] |= (uint32_t)bytes[column * 20 + i * 4 + j] << (8 * j);
    }
    memcpy(output, values, count * sizeof(*output));
    return true;
}

bool quest_camera_mcu_command_encode(enum quest_camera_mcu_operation operation,
                                      uint32_t camera_mask, uint8_t sequence, void *output, size_t size)
{
    if ((operation != QUEST_CAMERA_MCU_START && operation != QUEST_CAMERA_MCU_STOP &&
         operation != QUEST_CAMERA_MCU_INITIALIZE && operation != QUEST_CAMERA_MCU_DEINITIALIZE) || !camera_mask || !sequence || sequence == 255 ||
        !output || size < QUEST_CAMERA_MCU_COMMAND_SIZE) return false;
    uint8_t bytes[QUEST_CAMERA_MCU_COMMAND_SIZE] = {(uint8_t)operation, sequence, 4};
    for (unsigned i = 0; i < 4; ++i) bytes[3 + i] = (uint8_t)(camera_mask >> (8 * i));
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
bool quest_camera_mcu_status_decode(const void *payload, size_t size, int32_t *status)
{
    if (!payload || size != QUEST_CAMERA_MCU_STATUS_SIZE || !status) return false;
    const uint8_t *bytes = payload;
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)bytes[i] << (8 * i);
    memcpy(status, &value, sizeof(value));
    return true;
}

bool quest_camera_mcu_setting_encode(enum quest_camera_mcu_setting setting,
    const struct quest_camera_mcu_setting_record *records, size_t count,
    uint8_t sequence, void *output, size_t capacity, size_t *written)
{
    size_t width = 5;
    bool single = false;
    switch (setting) {
    case QUEST_CAMERA_MCU_BINNING:
    case QUEST_CAMERA_MCU_BPP:
    case QUEST_CAMERA_MCU_SYNC_MODE: break;
    case QUEST_CAMERA_MCU_IMAGE: width = 6; break;
    case QUEST_CAMERA_MCU_MIPI_CLOCK:
    case QUEST_CAMERA_MCU_METADATA_MODE:
    case QUEST_CAMERA_MCU_GAMMA: single = true; break;
    default: return false;
    }
    if (!records || !count || count > QUEST_CAMERA_MCU_SETTING_MAX_RECORDS ||
        (single && count != 1) || !sequence || sequence == 255 || !output || !written)
        return false;
    size_t length = 4 + count * width;
    if (capacity < length) return false;
    uint32_t seen = 0;
    uint8_t bytes[QUEST_CAMERA_MCU_SETTING_MAX_SIZE] = {3, sequence, (uint8_t)(length - 3), (uint8_t)setting};
    for (size_t i = 0; i < count; ++i) {
        const struct quest_camera_mcu_setting_record *record = &records[i];
        if (!record->camera_mask || (seen & record->camera_mask) ||
            (width == 5 && record->second) || (setting == QUEST_CAMERA_MCU_GAMMA && record->value > 1))
            return false;
        seen |= record->camera_mask;
        for (unsigned j = 0; j < 4; ++j)
            bytes[4 + i * width + j] = (uint8_t)(record->camera_mask >> (8 * j));
        bytes[8 + i * width] = record->value;
        if (width == 6) bytes[9 + i * width] = record->second;
    }
    memcpy(output, bytes, length);
    *written = length;
    return true;
}
