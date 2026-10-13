#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

typedef unsigned u32;
#define DSI_ERR(...) ((void)0)
struct regulator { unsigned index, load, enabled, enables, disables; };
struct dsi_vreg {
    struct regulator *vreg;
    char vreg_name[8];
    unsigned pre_on_sleep, post_on_sleep, pre_off_sleep, post_off_sleep;
    unsigned enable_load, disable_load, min_voltage, max_voltage, off_min_voltage;
};
struct dsi_regulator_info { int count; struct dsi_vreg *vregs; };
static int fail_at, failure, voltage_count;
static unsigned delays;
static int regulator_set_load(struct regulator *r, unsigned load) { r->load = load; return 0; }
static int regulator_count_voltages(struct regulator *r) { (void)r; return voltage_count; }
static int regulator_set_voltage(struct regulator *r, unsigned low, unsigned high)
{ (void)r; assert(low <= high); return 0; }
static void usleep_range(unsigned low, unsigned high) { assert(low && high == low + 10); ++delays; }
static int regulator_enable(struct regulator *r)
{
    ++r->enables;
    if ((int)r->index == fail_at) return failure;
    ++r->enabled;
    return 0;
}
static int regulator_disable(struct regulator *r)
{ assert(r->enabled); --r->enabled; ++r->disables; return 0; }

#include "driver.inc"

int main(void)
{
    unsigned cases = 0, defects = 0;
    const int errors[] = {-EIO, -ENOMEM, -517};
    for (int count = 1; count <= 8; count++) {
        for (int at = 0; at < count; at++) {
            for (unsigned e = 0; e < sizeof(errors) / sizeof(*errors); e++) {
                for (voltage_count = 0; voltage_count <= 1; voltage_count++) {
                    for (unsigned sleep = 0; sleep <= 1; sleep++) {
                        struct regulator regulators[8] = {{0}};
                        struct dsi_vreg rails[8] = {{0}};
                        for (int i = 0; i < count; i++) {
                            regulators[i].index = i;
                            regulators[i].load = i + 1;
                            rails[i] = (struct dsi_vreg){.vreg = &regulators[i], .enable_load = 100 + i,
                                .disable_load = i + 1, .min_voltage = 1000, .max_voltage = 2000,
                                .pre_on_sleep = sleep, .post_on_sleep = sleep,
                                .pre_off_sleep = sleep, .post_off_sleep = sleep};
                        }
                        struct dsi_regulator_info group = {count, rails};
                        fail_at = at; failure = errors[e]; delays = 0;
                        assert(dsi_pwr_enable_vregs(&group, true) == failure);
                        for (int i = 0; i < count; i++) {
                            defects += regulators[i].load != rails[i].disable_load;
                            assert(!regulators[i].enabled);
                            assert(regulators[i].enables == (unsigned)(i <= at));
                            assert(regulators[i].disables == (unsigned)(i < at));
                        }
                        assert(delays == (sleep ? (unsigned)(at * 4 + 1) : 0));
                        fail_at = -1;
                        assert(dsi_pwr_enable_vregs(&group, true) == 0);
                        for (int i = 0; i < count; i++) {
                            assert(regulators[i].enabled == 1);
                            assert(regulators[i].load == rails[i].enable_load);
                        }
                        assert(dsi_pwr_enable_vregs(&group, false) == 0);
                        for (int i = 0; i < count; i++) {
                            assert(!regulators[i].enabled);
                            assert(regulators[i].load == rails[i].disable_load);
                        }
                        ++cases;
                    }
                }
            }
        }
    }
#ifdef EXPECT_OLD
    assert(defects == cases);
#else
    assert(defects == 0);
#endif
    printf("PICO_PANEL_LOAD_PASS cases=%u load_defects=%u\n", cases, defects);
}
