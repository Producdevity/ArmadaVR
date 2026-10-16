#ifndef REFRAMED_QUEST_CAMERA_BLOB_H
#define REFRAMED_QUEST_CAMERA_BLOB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QUEST_CAMERA_CLOCK_BLOB_SIZE 60
#define QUEST_CAMERA_CSID_CLOCK_BLOB_SIZE 12
#define QUEST_CAMERA_HFR_BLOB_SIZE 36
#define QUEST_CAMERA_BANDWIDTH_BLOB_SIZE 68

struct quest_camera_clock_votes {
    uint32_t rdi;
    uint64_t pixel_hz, rdi_hz;
};
struct quest_camera_bandwidth_votes {
    uint32_t rdi;
    uint64_t camnoc_bps, mnoc_average_bps, mnoc_peak_bps;
    uint64_t ddr_average_bps, ddr_peak_bps;
};

/* Single-ISP, one RDI0..3 path. Generic blobs include the four-byte wire header.
 * Votes are explicit rates, not estimates or validated hardware operating points.
 * Bandwidth units are bytes/second. DDR votes may both be zero when unused.
 * HFR emits every frame, matching the single-image packet encoder.
 * No device I/O. Failure preserves output. */
bool quest_camera_clock_blob(const struct quest_camera_clock_votes *votes, void *output, size_t size);
bool quest_camera_csid_clock_blob(uint64_t rate_hz, void *output, size_t size);
bool quest_camera_hfr_blob(uint32_t rdi, void *output, size_t size);
bool quest_camera_bandwidth_blob(const struct quest_camera_bandwidth_votes *votes, void *output, size_t size);
#endif
