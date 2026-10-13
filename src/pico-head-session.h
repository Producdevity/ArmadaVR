#ifndef REFRAMED_PICO_HEAD_SESSION_H
#define REFRAMED_PICO_HEAD_SESSION_H

#include "pico-head-tracking.h"
#include "ssc-imu-session.h"

struct pico_head_session;

/* Supply verified raw/calibration SUIDs, display rigid-body identities, rates,
 * clock calibration and unit/axis conversion. Leave report, invalidate and
 * bias.report NULL: this owner connects corrected samples to orientation fusion.
 * sensors.data remains the argument for the supplied clock calibration callback.
 * Nothing discovers a physical identity or starts automatically at boot. */
bool pico_head_session_create(const struct ssc_imu_session_config *sensors,
                              const struct pico_head_tracking_config *tracking,
                              struct pico_head_session **session);
bool pico_head_session_get(struct pico_head_session *session, int64_t now_ns,
                           struct xrt_space_relation *relation, int64_t *sample_time_ns);
void pico_head_session_get_status(struct pico_head_session *session,
                                  struct ssc_imu_session_status *status);
void pico_head_session_stop(struct pico_head_session *session);
/* The owner must stop/join before freeing clock callback data or its devices. */
bool pico_head_session_destroy(struct pico_head_session **session);

#endif
