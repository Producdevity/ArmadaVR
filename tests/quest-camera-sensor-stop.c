#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define CAM_ERR(...) ((void)0)
#define CAM_INFO(...) ((void)0)
#define CAM_DBG(...) ((void)0)
#define CAM_WARN(...) ((void)0)
#define CAM_SOC_MAX_REGULATOR 8
#define MAX_POWER_CONFIG 32
#define INVALID_VREG 99
#define GPIOF_OUT_INIT_LOW 0

enum { SENSOR_MCLK, SENSOR_RESET, SENSOR_STANDBY, SENSOR_CUSTOM_GPIO1,
       SENSOR_CUSTOM_GPIO2, SENSOR_VANA, SENSOR_VANA1, SENSOR_VDIG, SENSOR_VIO, SENSOR_VAF,
       SENSOR_VAF_PWDM, SENSOR_CUSTOM_REG1, SENSOR_CUSTOM_REG2 };
struct regulator { unsigned id; };
struct cam_sensor_power_setting { unsigned seq_type; unsigned short seq_val, delay; long config_val; void *data[10]; };
struct msm_camera_gpio_num_info { bool valid[16]; int gpio_num[16]; };
struct cam_sensor_power_ctrl_t {
    struct msm_camera_gpio_num_info *gpio_num_info;
    struct cam_sensor_power_setting *power_down_setting;
    uint16_t power_down_setting_size;
    int cam_pinctrl_status;
    struct { void *pinctrl, *gpio_state_suspend; } pinctrl_info;
};
struct cam_hw_soc_info {
    int num_rgltr, num_clk;
    struct regulator *rgltr[8];
    const char *rgltr_name[8];
    int rgltr_min_volt[8], rgltr_max_volt[8], rgltr_op_mode[8], rgltr_delay[8];
    void *clk[2]; const char *clk_name[2];
    bool use_shared_clk;
};
struct camera_io_master { int unused; };
struct cam_sensor_board_info { struct cam_sensor_power_ctrl_t power_info; };
struct cam_sensor_ctrl_t {
    struct cam_sensor_board_info *sensordata;
    struct cam_hw_soc_info soc_info;
    bool bob_pwm_switch, hw_no_ops, hw_no_io_ops; unsigned bob_reg_index;
    struct camera_io_master io_master_info;
};
static unsigned cases, defects, mask, trace[128], count, bob_calls, io_calls;
static int io_error, bob_error;
static struct cam_sensor_power_setting settings[4];
static void check(bool value) { defects += !value; }
static void record(unsigned value) { if (count < 128) trace[count++] = value; else check(false); }
static int error(unsigned bit) { return mask & (1U << bit) ? -(int)(100 + bit) : 0; }
static void cam_soc_util_clk_disable(struct cam_hw_soc_info *soc, bool include_shared, int index)
{ check(soc && !include_shared && index >= 0 && index < 2); record(index + 1); }
static int cam_config_mclk_reg(struct cam_sensor_power_ctrl_t *c, struct cam_hw_soc_info *s, int i)
{ check(c && s && i == 0); record(3); return error(0); }
static void cam_res_mgr_gpio_set_value(int gpio, int value) { check(gpio == 10 && !value); record(20); }
static struct cam_sensor_power_setting *msm_camera_get_power_settings(
    struct cam_sensor_power_ctrl_t *c, unsigned type, unsigned value)
{ check(c && value < 2 && type == (value ? SENSOR_VIO : SENSOR_VANA)); return &settings[value + 1]; }
static int msm_cam_sensor_handle_reg_gpio(unsigned type, struct msm_camera_gpio_num_info *g, int value)
{ check(g && !value); record(type == SENSOR_VANA ? 10 : 11); return 0; }
static int cam_soc_util_regulator_disable(struct regulator *r, const char *name, int low, int high, int load, int delay)
{ check(r && name && !low && high == 1000 && !load && !delay); record(12 + r->id); return error(r->id ? 2 : 1); }
static void msleep(unsigned ms) { record(1000 + ms); }
static void usleep_range(unsigned low, unsigned high) { check(high == low + 1000); record(1000 + low / 1000); }
static int pinctrl_select_state(void *p, void *state) { check(p && state); record(21); return error(3); }
static void devm_pinctrl_put(void *p) { check(p != NULL); record(22); }
static int cam_sensor_util_request_gpio_table(struct cam_hw_soc_info *soc, int on)
{ check(soc && !on); record(26); return mask & 16 ? -EINVAL : 0; }
static int cam_sensor_bob_pwm_mode_switch(struct cam_hw_soc_info *soc, unsigned index, bool on)
{ check(soc && !index && !on); ++bob_calls; return bob_error; }
static int camera_io_release(struct camera_io_master *io) { check(io != NULL); ++io_calls; return io_error; }

#include "techpack/camera/drivers/cam_sensor_module/cam_sensor_utils/cam_sensor_util.c"
#include "techpack/camera/drivers/cam_sensor_module/cam_sensor/cam_sensor_core.c"

static void first_error(int *result, int value) { if (!*result) *result = value; }
int main(void)
{
    const unsigned delays[] = {0, 1, 20, 21};
    for (unsigned flags = 0; flags < 4; ++flags) for (mask = 0; mask < 32; ++mask)
        for (unsigned delay = 0; delay < 4; ++delay) {
            struct regulator regs[2] = {{0}, {1}};
            struct msm_camera_gpio_num_info gpio = {.valid = {[SENSOR_RESET] = true},
                .gpio_num = {[SENSOR_RESET] = 10}};
            struct cam_sensor_ctrl_t sensor = {.soc_info = {.num_rgltr = 2,
                .num_clk = flags & 1 ? 2 : 0, .rgltr = {regs, regs+1},
                .rgltr_name = {"a", "b"}, .rgltr_max_volt = {1000, 1000},
                .clk = {(void *)1, (void *)2}, .clk_name = {"a", "b"}, .use_shared_clk = flags & 4}};
            struct cam_sensor_power_ctrl_t power = {.gpio_num_info = &gpio,
                .power_down_setting = settings, .power_down_setting_size = 4,
                .cam_pinctrl_status = flags & 2,
                .pinctrl_info = {&gpio, &power}};
            settings[0] = (struct cam_sensor_power_setting){.seq_type = SENSOR_MCLK, .delay = delays[delay]};
            settings[1] = (struct cam_sensor_power_setting){.seq_type = SENSOR_VANA, .delay = delays[delay]};
            settings[2] = (struct cam_sensor_power_setting){.seq_type = SENSOR_VIO, .seq_val = 1, .delay = delays[delay]};
            settings[3] = (struct cam_sensor_power_setting){.seq_type = SENSOR_RESET, .delay = delays[delay]};
            unsigned expected_trace[128], n = 0;
            int expected = error(0);
            if (flags & 1) { expected_trace[n++] = 2; expected_trace[n++] = 1; }
            expected_trace[n++] = 3;
            if (!error(0) && delay) expected_trace[n++] = 1000 + delays[delay];
            for (unsigned rail = 0; rail < 2; ++rail) {
                expected_trace[n++] = 12 + rail;
                first_error(&expected, error(rail ? 2 : 1));
                expected_trace[n++] = 10 + rail;
                if (!error(rail ? 2 : 1) && delay) expected_trace[n++] = 1000 + delays[delay];
            }
            expected_trace[n++] = 20;
            if (delay) expected_trace[n++] = 1000 + delays[delay];
            if (flags & 2) { expected_trace[n++] = 21; expected_trace[n++] = 22; first_error(&expected, error(3)); }
            expected_trace[n++] = 26;
            count = 0;
            check(cam_sensor_util_power_down(&power, &sensor.soc_info) == expected);
            check(count == n && !memcmp(trace, expected_trace, n * sizeof(*trace)));
            check(!power.cam_pinctrl_status && sensor.soc_info.rgltr[0] == regs && sensor.soc_info.rgltr[1] == regs + 1);
            ++cases;
            if (delay) continue;
            for (unsigned mode = 0; mode < 32; ++mode) {
                struct cam_sensor_board_info data = {power};
                sensor.sensordata = &data;
                data.power_info.cam_pinctrl_status = flags & 2;
                sensor.soc_info.rgltr[0] = regs; sensor.soc_info.rgltr[1] = regs+1;
                sensor.bob_pwm_switch = mode & 1;
                bob_error = mode & 2 ? -ENOTSUP : 0; io_error = mode & 4 ? -EIO : 0;
                count = bob_calls = io_calls = 0;
                sensor.hw_no_ops = mode & 8;
                sensor.hw_no_io_ops = mode & 16;
                int result = sensor.hw_no_ops ? 0 : expected ? expected : sensor.hw_no_io_ops ? 0 : io_error;
                check(cam_sensor_power_down(&sensor) == result);
                unsigned calls = sensor.hw_no_ops ? 0 : n;
                check(count == calls && !memcmp(trace, expected_trace, calls * sizeof(*trace)));
                check(bob_calls == (!sensor.hw_no_ops && !expected && (mode & 1)));
                check(io_calls == (!sensor.hw_no_ops && !sensor.hw_no_io_ops && !expected));
                ++cases;
            }
        }
    count = 0;
    check(cam_sensor_util_power_down(NULL, NULL) == -EINVAL && !count);
    check(cam_sensor_power_down(NULL) == -EINVAL && !count);
    cases += 2;
    printf("QUEST_CAMERA_SENSOR_STOP cases=%u defects=%u\n", cases, defects);
    return defects != 0;
}
