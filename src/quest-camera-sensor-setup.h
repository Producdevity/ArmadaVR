#ifndef REFRAMED_QUEST_CAMERA_SENSOR_SETUP_H
#define REFRAMED_QUEST_CAMERA_SENSOR_SETUP_H
#include "quest-camera-sensor.h"
#include "quest-camera-allocation.h"

struct quest_camera_sensor_setup;
struct quest_camera_sensor_table {
    struct quest_camera_sensor_bus bus;
    const struct quest_camera_sensor_write *writes;
    size_t count;
};
struct quest_camera_sensor_setup_config {
    int crm_fd, sensor_fd;
    int32_t session, sensor;
    struct quest_camera_sensor_table initialize, mode, synchronize, stream_on, stream_off;
};
enum quest_camera_sensor_stage {
    QUEST_CAMERA_SENSOR_INITIALIZE, QUEST_CAMERA_SENSOR_MODE, QUEST_CAMERA_SENSOR_SYNCHRONIZE,
    QUEST_CAMERA_SENSOR_STREAM_ON, QUEST_CAMERA_SENSOR_STREAM_OFF, QUEST_CAMERA_SENSOR_STAGES,
};

/* Copies explicit tables into owned CRM command/packet memory. Sensor and CRM
 * descriptors and handles are borrowed exclusively. Only mode/synchronize may
 * be empty (retained Quest mode tables are empty). No module, operating point,
 * synchronization role or shutdown sequence is inferred. A nonnull owner must
 * survive failure until release or independently completed cleanup. */
int quest_camera_sensor_setup_prepare(const struct quest_camera_sensor_setup_config *config,
                                      struct quest_camera_sensor_setup **setup);
/* One configure attempt: initialize, optional mode/sync, stream-on/off tables.
 * No CAM_START_DEV is sent, but register writes can themselves affect streaming.
 * The retained Quest stream-off table is not a hardware shutdown sequence; MCU
 * control and independent quiescence are still required. Never retries. */
int quest_camera_sensor_setup_submit(struct quest_camera_sensor_setup *setup);
/* All controls must have returned; independently confirm DMA/CPU/device
 * quiescence. Required after submit or any partial preparation failure. */
int quest_camera_sensor_setup_quiesced(struct quest_camera_sensor_setup *setup);
int quest_camera_sensor_setup_release(struct quest_camera_sensor_setup **setup);
/* Cleanup inspection only. Returns false for an absent stage/allocation. */
bool quest_camera_sensor_setup_resources(const struct quest_camera_sensor_setup *setup,
    enum quest_camera_sensor_stage stage, bool packet, struct quest_camera_allocation_view *view);
/* Local disposal only after all mappings, owned fds and CRM resources have been
 * externally cleaned up with independent quiescence. Does not perform I/O. */
void quest_camera_sensor_setup_closed(struct quest_camera_sensor_setup **setup);
#endif
