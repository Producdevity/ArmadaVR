#include "quest-camera-blob.h"
#include <string.h>

static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}
static void put64(uint8_t *bytes, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}
bool quest_camera_clock_blob(const struct quest_camera_clock_votes *v, void *output, size_t size)
{
    if (!v || !output || size < QUEST_CAMERA_CLOCK_BLOB_SIZE || v->rdi > 3 || !v->pixel_hz || !v->rdi_hz)
        return false;
    uint8_t bytes[QUEST_CAMERA_CLOCK_BLOB_SIZE] = {0};
    put32(bytes, (56u << 8) | 1);
    put32(bytes + 8, 4);
    put64(bytes + 12, v->pixel_hz);
    put64(bytes + 28 + v->rdi * 8, v->rdi_hz);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
bool quest_camera_csid_clock_blob(uint64_t rate_hz, void *output, size_t size)
{
    if (!rate_hz || !output || size < QUEST_CAMERA_CSID_CLOCK_BLOB_SIZE) return false;
    uint8_t bytes[QUEST_CAMERA_CSID_CLOCK_BLOB_SIZE] = {0};
    put32(bytes, (8u << 8) | 4);
    put64(bytes + 4, rate_hz);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
bool quest_camera_hfr_blob(uint32_t rdi, void *output, size_t size)
{
    if (rdi > 3 || !output || size < QUEST_CAMERA_HFR_BLOB_SIZE) return false;
    uint8_t bytes[QUEST_CAMERA_HFR_BLOB_SIZE] = {0};
    put32(bytes, 32u << 8);
    put32(bytes + 4, 1);
    put32(bytes + 12, 0x3006 + rdi);
    put32(bytes + 16, 1);
    put32(bytes + 24, 1);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
bool quest_camera_bandwidth_blob(const struct quest_camera_bandwidth_votes *v, void *output, size_t size)
{
    if (!v || !output || size < QUEST_CAMERA_BANDWIDTH_BLOB_SIZE || v->rdi > 3 ||
        !v->camnoc_bps || !v->mnoc_average_bps || v->mnoc_peak_bps < v->mnoc_average_bps ||
        v->ddr_peak_bps < v->ddr_average_bps || (!v->ddr_average_bps != !v->ddr_peak_bps)) return false;
    uint8_t bytes[QUEST_CAMERA_BANDWIDTH_BLOB_SIZE] = {0};
    put32(bytes, (64u << 8) | 9);
    put32(bytes + 8, 1);
    put32(bytes + 12, 3);
    put32(bytes + 16, 1);
    put32(bytes + 20, 4 + v->rdi);
    put64(bytes + 28, v->camnoc_bps);
    put64(bytes + 36, v->mnoc_average_bps);
    put64(bytes + 44, v->mnoc_peak_bps);
    put64(bytes + 52, v->ddr_average_bps);
    put64(bytes + 60, v->ddr_peak_bps);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}
