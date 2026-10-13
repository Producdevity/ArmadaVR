#ifndef ARMADA_SSC_IMU_SESSION_H
#define ARMADA_SSC_IMU_SESSION_H

#include "ssc-calibration.h"

struct ssc_imu_session;

struct ssc_imu_session_bias_config {
    uint64_t suid_high[2], suid_low[2];
    int rigid_body[2];
    int64_t max_age_ns;
    void (*report)(void *data, const struct ssc_bias_sample *sample);
};

struct ssc_imu_session_config {
    uint64_t suid_high[2], suid_low[2];
    float sample_rate[2];
    int64_t max_age_ns, startup_timeout_ns, report_timeout_ns;
    bool (*calibrate)(void *data, struct ssc_clock *clock);
    void (*report)(void *data, const struct ssc_imu_event *event);
    void (*invalidate)(void *data);
    void *data;
    struct ssc_imu_session_bias_config bias;
};

struct ssc_imu_session_status {
    uint64_t accepted[2], rejected[2];
    uint64_t calibration_accepted[2], calibration_rejected[2], corrected[2];
    bool streaming, stopping, finished, failed;
    char error[192];
};

/* Discover accel/gyro on one private worker, then require the supplied SUIDs,
 * available streaming sensors and exact advertised rates before opening either.
 * Callbacks run on that worker; data must remain alive until destroy succeeds.
 * report receives a borrowed event only while both streams are open. invalidate
 * runs once when stopping, including failed startup. No retries or pose output.
 * A non-NULL bias.report opts into two additional calibration subscriptions.
 * All four exact SUIDs and each pair's expected rigid body must match before
 * enabling any sensor. Raw callbacks then wait for all four subscriptions;
 * corrected callbacks additionally require a reliable, timely bias. Losing
 * corrected output for report_timeout_ns invalidates the whole session.
 * A single initial on-change calibration report is retained during startup.
 * The initial output must be NULL; configuration is copied. */
bool ssc_imu_session_create(const struct ssc_imu_session_config *config,
                            struct ssc_imu_session **session);
void ssc_imu_session_get_status(struct ssc_imu_session *session,
                                struct ssc_imu_session_status *status);
void ssc_imu_session_stop(struct ssc_imu_session *session);

/* Cancel, disable both sensors and drain callbacks before freeing. Do not race
 * destroy with public calls. Calling it from a worker callback returns false
 * without freeing; the owner must join later. NULL is already destroyed. */
bool ssc_imu_session_destroy(struct ssc_imu_session **session);

#endif
