#ifndef REFRAMED_QUEST_CAMERA_ISP_H
#define REFRAMED_QUEST_CAMERA_ISP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QUEST_CAMERA_ISP_ACQUIRE_SIZE 216
#define QUEST_CAMERA_PHY_CONFIG_SIZE 24

enum quest_camera_raw_format { QUEST_CAMERA_RAW_MIPI, QUEST_CAMERA_RAW_PLAIN8, QUEST_CAMERA_RAW_PLAIN16 };
struct quest_camera_isp_config {
    uint32_t phy, cphy, combo, lanes, lane_map, virtual_channel, bits;
    uint32_t width, height, rdi;
    enum quest_camera_raw_format output;
};
struct quest_camera_phy_config {
    uint32_t cphy, combo, lanes, lane_map, settle_count, mipi_flags;
    uint64_t data_rate;
};
/* Encode an explicit uncropped RAW8/RAW10 input and one RDI0..3 output using
 * Quest's supported v2 acquire ABI. The PHY payload is separate and uses the
 * same mode chosen at acquisition. Lane wiring, rate, settle count and mode
 * must come from verified hardware configuration. Mixed/remote PHY, cropping,
 * binning and SFE paths are excluded. Failure leaves output unchanged; neither
 * function performs I/O or establishes a safe hardware operating point. */
bool quest_camera_isp_encode(const struct quest_camera_isp_config *config, void *output, size_t size);
bool quest_camera_phy_encode(const struct quest_camera_phy_config *config, void *output, size_t size);
#endif
