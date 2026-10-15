#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define CAM_ERR(...) ((void)0)
struct regulator { unsigned id; int disable, load, voltage, voltages; };
struct cam_hw_soc_info {
    uint32_t num_rgltr;
    bool rgltr_ctrl_support;
    struct regulator *rgltr[8];
    const char *rgltr_name[8];
    uint32_t rgltr_min_volt[8], rgltr_max_volt[8], rgltr_op_mode[8], rgltr_delay[8];
    struct { void *pinctrl, *gpio_state_suspend; } pinctrl_info;
};
static unsigned cases, defects, trace[64], trace_count, delays;
static int irq_result;
static void check(bool value) { defects += !value; }
static void record(unsigned value) { if (trace_count < 64) trace[trace_count++] = value; else check(false); }
static int regulator_disable(struct regulator *r) { record(r->id * 4); return r->disable; }
static int regulator_count_voltages(struct regulator *r) { record(r->id * 4 + 1); return r->voltages; }
static int regulator_set_load(struct regulator *r, int load)
{ check(!load); record(r->id * 4 + 2); return r->load; }
static int regulator_set_voltage(struct regulator *r, int low, int high)
{ check(!low && high == 1000); record(r->id * 4 + 3); return r->voltage; }
static void msleep(unsigned ms) { check(ms == 21); ++delays; }
static void usleep_range(unsigned low, unsigned high)
{ check((low == 1000 || low == 20000) && high == low + 1000); ++delays; }
static int cam_soc_util_irq_disable(struct cam_hw_soc_info *soc)
{ check(soc != NULL); record(40); return irq_result; }
static void cam_soc_util_clk_disable_default(struct cam_hw_soc_info *soc)
{ check(soc != NULL); record(41); }

#include "techpack/camera/drivers/cam_utils/cam_soc_util.c"

static void first_error(int *result, int value) { if (!*result) *result = value; }
int main(void)
{
    const unsigned delay_values[] = {0, 1, 20, 21};
    for (unsigned mask = 0; mask < 8; ++mask)
        for (unsigned voltage = 0; voltage < 2; ++voltage)
            for (unsigned delay = 0; delay < 4; ++delay) {
                struct regulator reg = {0, mask & 1 ? -EIO : 0,
                    mask & 2 ? -EINTR : 0, mask & 4 ? -ERANGE : 0, (int)voltage};
                trace_count = delays = 0;
                int expected = reg.disable ? reg.disable : voltage ? (reg.load ? reg.load : reg.voltage) : 0;
                check(cam_soc_util_regulator_disable(&reg, "test", 0, 1000, 0, delay_values[delay]) == expected);
                check(trace_count == (reg.disable ? 1U : voltage ? 4U : 2U));
                check(delays == (!reg.disable && delay > 0));
                ++cases;
            }
    trace_count = 0;
    check(cam_soc_util_regulator_disable(NULL, "test", 0, 1000, 0, 0) == -EINVAL && !trace_count);
    check(cam_soc_util_disable_platform_resource(NULL, true, true) == -EINVAL && !trace_count);
    cases += 2;
    for (unsigned count = 0; count <= 8; ++count)
        for (unsigned control = 0; control < 2; ++control)
            for (unsigned flags = 0; flags < 4; ++flags)
                    for (unsigned voltages = 0; voltages < 2; ++voltages)
                        for (unsigned mask = 0; mask < 16; ++mask) {
                            struct regulator regs[8];
                            struct cam_hw_soc_info soc = {.num_rgltr = count, .rgltr_ctrl_support = control};
                            irq_result = mask & 1 ? -ENODEV : 0;
                            for (unsigned i = 0; i < count; ++i) {
                                regs[i] = (struct regulator){i, mask & 2 ? (i % 2 ? -EIO : -ETIMEDOUT) : 0,
                                    mask & 4 ? -EINTR : 0, mask & 8 ? -ERANGE : 0, (int)voltages};
                                soc.rgltr[i] = &regs[i]; soc.rgltr_name[i] = "test"; soc.rgltr_max_volt[i] = 1000;
                            }
                            unsigned expected_trace[64], n = 0;
                            int expected = 0;
                            if (flags & 1) { expected_trace[n++] = 40; first_error(&expected, irq_result); }
                            if (flags & 2) expected_trace[n++] = 41;
                            for (unsigned i = count; i-- > 0;) {
                                expected_trace[n++] = i * 4;
                                int failure = regs[i].disable;
                                if (control && !failure) {
                                    expected_trace[n++] = i * 4 + 1;
                                    if (voltages) {
                                        expected_trace[n++] = i * 4 + 2;
                                        expected_trace[n++] = i * 4 + 3;
                                        failure = regs[i].load ? regs[i].load : regs[i].voltage;
                                    }
                                }
                                first_error(&expected, failure);
                            }
                            trace_count = delays = 0;
                            check(cam_soc_util_disable_platform_resource(&soc, flags & 2, flags & 1) == expected);
                            check(trace_count == n && !memcmp(trace, expected_trace, n * sizeof(*trace)));
                            check(!delays);
                            ++cases;
                        }
    for (unsigned control = 0; control < 2; ++control) {
        struct cam_hw_soc_info soc = {.num_rgltr = 1, .rgltr_ctrl_support = control};
        trace_count = 0;
        check(cam_soc_util_disable_platform_resource(&soc, false, false) == (control ? -EINVAL : 0));
        check(!trace_count); ++cases;
    }
    printf("QUEST_CAMERA_POWER_STOP cases=%u defects=%u\n", cases, defects);
    return defects != 0;
}
