#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#define DSI_ERR(...) ((void)0)
#define DSI_DEBUG(...) ((void)0)
struct dsi_vreg { const char *vreg_name; };
struct dsi_regulator_info { unsigned count, refcount; struct dsi_vreg *vregs; };
static int enable_error;
static unsigned enables, disables;
static bool powered;

static int dsi_pwr_enable_vregs(struct dsi_regulator_info *regs, bool enable)
{
    assert(regs->count && regs->vregs);
    if (enable) {
        ++enables;
        if (enable_error)
            return enable_error;
        powered = true;
    } else {
        ++disables;
        powered = false;
    }
    return 0;
}

#include "driver.inc"

int main(void)
{
    struct dsi_vreg vreg = {"panel"};
    const int errors[] = {-EIO, -ENOMEM, -517};
    unsigned cases = 0, defects = 0;
    for (unsigned e = 0; e < sizeof(errors) / sizeof(errors[0]); ++e) {
        for (unsigned failures = 1; failures <= 16; ++failures) {
            for (unsigned users = 1; users <= 16; ++users) {
                struct dsi_regulator_info group = {1, 0, &vreg};
                enables = disables = 0; powered = false;
                enable_error = errors[e];
                for (unsigned i = 0; i < failures; ++i) {
                    int rc = dsi_pwr_enable_regulator(&group, true);
                    defects += rc != errors[e] || group.refcount != 0 || powered || enables != i + 1;
                }
                enable_error = 0;
                for (unsigned i = 0; i < users; ++i) {
                    int rc = dsi_pwr_enable_regulator(&group, true);
                    defects += rc != 0 || !powered || group.refcount != i + 1 || enables != failures + 1;
                }
                for (unsigned i = 0; i < users; ++i) {
                    int rc = dsi_pwr_enable_regulator(&group, false);
                    defects += rc != 0 || group.refcount != users - i - 1 || powered != (i + 1 < users);
                }
                defects += disables != 1;
                ++cases;

            }
        }
    }
    struct dsi_regulator_info empty = {0, 0, NULL};
    assert(dsi_pwr_enable_regulator(&empty, true) == 0 && empty.refcount == 0);
    assert(dsi_pwr_enable_regulator(&empty, false) == 0 && empty.refcount == 0);
    struct dsi_regulator_info invalid = {1, 0, NULL};
    assert(dsi_pwr_enable_regulator(&invalid, true) == -EINVAL && invalid.refcount == 0);
    enables = disables = 0;
    struct dsi_regulator_info active = {1, 1, &vreg};
    powered = true; enable_error = -EIO;
    assert(dsi_pwr_enable_regulator(&active, true) == 0 && active.refcount == 2 && !enables);
    assert(dsi_pwr_enable_regulator(&active, false) == 0 && active.refcount == 1 && !disables);
    assert(dsi_pwr_enable_regulator(&active, false) == 0 && active.refcount == 0 && disables == 1 && !powered);
#ifdef EXPECT_OLD
    assert(defects > 0);
#else
    assert(defects == 0);
#endif
    printf("%u regulator retry sequences; %u reference defects\n", cases, defects);
    return 0;
}
