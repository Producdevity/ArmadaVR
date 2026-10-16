#include "quest-camera-sensor.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <media/cam_sensor.h>
#define U16_MAX UINT16_MAX
#define GFP_KERNEL 0
#define kzalloc(size, flags) calloc(1, size)
#define kfree free
#define CAM_ERR(...) ((void)0)
#define CAM_WARN(...) ((void)0)
#define CAM_DBG(...) ((void)0)
#include "quest-camera-power-kernel.inc"

int main(void)
{
    _Static_assert(MAX_POWER_CONFIG == 12 && SENSOR_SEQ_TYPE_MAX == 13 && SENSOR_VANA1 == 12, "Quest power limits");
    unsigned cases = 0;
    for (unsigned count = 1; count <= 12; ++count)
    for (unsigned delays = 0; delays < (1u << count); ++delays)
    for (unsigned rotation = 0; rotation < 13; ++rotation) {
        struct quest_camera_sensor_power_step up[12], down[12];
        for (unsigned i = 0; i < count; ++i) {
            up[i] = (struct quest_camera_sensor_power_step){(rotation + i) % 13, UINT32_MAX - i,
                delays & (1u << i) ? (i % 2 ? 32767 : 1) : 0};
        }
        for (unsigned i = 0; i < count; ++i) down[i] = up[count - i - 1];
        unsigned char bytes[704] = {0};
        size_t up_size = quest_camera_sensor_power(true, up, count, bytes, sizeof(bytes));
        size_t down_size = quest_camera_sensor_power(false, down, count, bytes + up_size, sizeof(bytes) - up_size);
        assert(up_size && down_size);
        struct cam_sensor_power_ctrl_t parsed = {0};
        assert(!cam_sensor_update_power_settings(bytes, up_size + down_size, &parsed, up_size + down_size));
        assert(parsed.power_setting_size == count && parsed.power_down_setting_size == count);
        for (unsigned i = 0; i < count; ++i) {
            assert((unsigned)parsed.power_setting[i].seq_type == up[i].sequence_type);
            assert(parsed.power_setting[i].config_val == (long)up[i].value && parsed.power_setting[i].delay == up[i].delay_ms);
            assert((unsigned)parsed.power_down_setting[i].seq_type == down[i].sequence_type);
            assert(parsed.power_down_setting[i].config_val == (long)down[i].value && parsed.power_down_setting[i].delay == down[i].delay_ms);
        }
        free(parsed.power_setting); free(parsed.power_down_setting); ++cases;
    }
    printf("QUEST_CAMERA_POWER_KERNEL_PASS cases=%u hardware=0\n", cases);
}
