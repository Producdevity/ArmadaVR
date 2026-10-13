#ifndef REFRAMED_IMU_CALIBRATION_H
#define REFRAMED_IMU_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>

struct imu_temperature_offset {
    float temperature;
    float offset[3];
};

/* Rows must be finite and strictly increasing in temperature. Outside the
 * table, use the nearest endpoint. A single row represents a constant offset.
 * The caller must preserve units and coordinate frames when preparing rows. */
bool imu_offset_at_temperature(const struct imu_temperature_offset *rows, size_t count,
                               float temperature, float offset[3]);

/* Apply row-major rectification * (raw - offset). This does not apply device
 * extrinsics, timestamp correction, or any second calibration model.
 * Both functions leave output unchanged on invalid input or arithmetic error. */
bool imu_rectify_vector(const float rectification[9], const float offset[3],
                        const float raw[3], float calibrated[3]);

/* Split a row-major rectification matrix as rotation * correction, where
 * rotation is proper orthonormal and correction is upper triangular with a
 * positive diagonal. Reject reflections, singular/ill-conditioned matrices
 * and non-finite results without changing either output. Outputs must not
 * overlap each other. This does not select a calibration frame or model. */
bool imu_split_rectification(const double rectification[9], double rotation[9],
                             double correction[9]);

enum imu_rectification_frame {
    IMU_FRAME_ORIGINAL,
    IMU_FRAME_ACCELEROMETER,
};

struct imu_rectification {
    float accelerometer[9];
    float gyroscope[9];
    double device_from_imu[9];
};

/* All matrices are row-major. The device rotation must already be proper
 * orthonormal. Accelerometer alignment factors Ma = Qa * Ra and produces
 * Ra, transpose(Qa) * Mg, and D * Qa. Original mode preserves Ma, Mg and D.
 * Offsets remain in the raw sensor frame; translation is unchanged. This does
 * not choose factory/online calibration or the active runtime frame. Reject
 * invalid or unrepresentable matrices without changing result. */
bool imu_prepare_rectification(const double accelerometer[9], const double gyroscope[9],
                               const double device_from_imu[9], enum imu_rectification_frame frame,
                               struct imu_rectification *result);

#endif
