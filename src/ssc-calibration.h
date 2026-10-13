#ifndef REFRAMED_SSC_CALIBRATION_H
#define REFRAMED_SSC_CALIBRATION_H

#include "ssc-imu.h"

#define SSC_CALIBRATION_HISTORY 32

struct ssc_calibration_value {
    float bias[3], scale[3], matrix[9];
    unsigned accuracy;
    uint32_t cal_id;
    bool has_scale, has_matrix, has_cal_id;
};

struct ssc_calibration_entry {
    struct ssc_calibration_value value;
    uint64_t ticks;
};

struct ssc_calibration_config {
    struct ssc_clock clock;
    uint64_t suid_high[2], suid_low[2];
    int64_t max_report_age_ns, max_bias_age_ns;
};

struct ssc_calibration {
    struct ssc_calibration_config config;
    struct ssc_calibration_entry entries[2][SSC_CALIBRATION_HISTORY];
    unsigned count[2];
    uint64_t generation;
    int64_t last_received_ns;
    bool active;
};

struct ssc_bias_sample {
    struct ssc_imu_event raw;
    struct ssc_calibration_entry calibration;
    float corrected[3];
    unsigned accuracy;
};

/* sns_cal_event: three finite biases, optional 3-vector scale/9-value matrix,
 * required accuracy 0..3, optional fixed32 ID. Absent arrays stay absent.
 * Packed/unpacked fields may be mixed. Output is unchanged on rejection. */
bool ssc_calibration_decode(const void *payload, size_t length,
                            struct ssc_calibration_value *value);

/* Zero-initialize once; use one thread. Pair calibration SUIDs with their
 * physical IMUs before calling. Use the IMU generation and clock; restarting
 * requires a strictly newer generation and clears both histories. */
bool ssc_calibration_begin(struct ssc_calibration *calibration,
                           const struct ssc_calibration_config *config, uint64_t generation);
void ssc_calibration_invalidate(struct ssc_calibration *calibration);
bool ssc_calibration_report(struct ssc_calibration *calibration, uint64_t generation,
                            uint32_t id, uint64_t high, uint64_t low, uint64_t ticks,
                            const void *payload, size_t length, int64_t now_ns);

/* Explicit stock-HAL bias subtraction, not factory matrix/axis correction.
 * Use only for uncorrected samples paired to this calibration stream. Select
 * the newest update at/before the sample; never use a future or stale bias,
 * skip an unreliable update, or substitute zero for a missing calibration.
 * Raw sample and calibration metadata are retained. No state changes here. */
bool ssc_calibration_correct(const struct ssc_calibration *calibration,
                             const struct ssc_imu_event *sample, int64_t now_ns,
                             struct ssc_bias_sample *output);

#endif
