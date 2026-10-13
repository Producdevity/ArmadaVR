#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
#define DSI_ERR(...) ((void)0)
#define DSI_DEBUG(...) ((void)0)
enum operation { LOAD_ON, VOLT_ON, ENABLE, VOLT_OFF, LOAD_OFF, DISABLE, OP_COUNT };
struct regulator {
    unsigned index, references, load, voltage, enables, disables;
};
#include "types.inc"
static struct regulator hardware[8];
static struct dsi_vreg rails[8];
static struct dsi_regulator_info group;
static int faults[8][OP_COUNT], voltage_count;
static unsigned cases, defects, unbalanced, delays;
static unsigned trace[256], trace_count;

static void expect(bool valid, int line)
{
    if (valid) return;
    ++defects;
#ifndef EXPECT_OLD
    if (defects < 20) fprintf(stderr, "case=%u line=%d failed\n", cases, line);
#else
    (void)line;
#endif
}
#define EXPECT(condition) expect(!!(condition), __LINE__)

static int operation(struct regulator *r, enum operation op)
{
    assert(trace_count < sizeof(trace) / sizeof(*trace));
    trace[trace_count++] = r->index * OP_COUNT + op;
    return faults[r->index][op];
}
static int regulator_set_load(struct regulator *r, unsigned load)
{
    r->load = load;
    return operation(r, load == rails[r->index].enable_load ? LOAD_ON : LOAD_OFF);
}
static int regulator_count_voltages(struct regulator *r) { (void)r; return voltage_count; }
static int regulator_set_voltage(struct regulator *r, unsigned low, unsigned high)
{
    assert(low <= high);
    int result = operation(r, low == rails[r->index].min_voltage ? VOLT_ON : VOLT_OFF);
    if (!result) r->voltage = low;
    return result;
}
static int regulator_enable(struct regulator *r)
{
    ++r->enables;
    int result = operation(r, ENABLE);
    if (!result) ++r->references;
    return result;
}
static int regulator_disable(struct regulator *r)
{
    ++r->disables;
    int result = operation(r, DISABLE);
    if (!r->references) { ++unbalanced; return -EIO; }
    if (!result) --r->references;
    return result;
}
static void usleep_range(unsigned low, unsigned high)
{ assert(low && high == low + 10); ++delays; }

#include "driver.inc"

static void setup(unsigned count, bool sleeps, bool off_voltage)
{
    memset(hardware, 0, sizeof(hardware));
    memset(rails, 0, sizeof(rails));
    memset(&group, 0, sizeof(group));
    memset(faults, 0, sizeof(faults));
    group.vregs = rails; group.count = count;
    unbalanced = delays = trace_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        hardware[i].index = i; hardware[i].load = i + 1;
        rails[i].vreg = &hardware[i];
        rails[i].enable_load = 100 + i; rails[i].disable_load = i + 1;
        rails[i].min_voltage = 1000; rails[i].max_voltage = 2000;
        rails[i].off_min_voltage = off_voltage ? 100 : 0;
        rails[i].pre_on_sleep = rails[i].post_on_sleep = sleeps;
        rails[i].pre_off_sleep = rails[i].post_off_sleep = sleeps;
    }
    ++cases;
}
static unsigned enable_calls(void)
{
    unsigned total = 0;
    for (unsigned i = 0; i < group.count; ++i) total += hardware[i].enables;
    return total;
}
static void off_state(void)
{
    EXPECT(group.refcount == 0);
    EXPECT(!unbalanced);
    for (unsigned i = 0; i < group.count; ++i) {
        EXPECT(hardware[i].references == 0);
        EXPECT(hardware[i].load == rails[i].disable_load);
    }
}
static void retry_success(void)
{
    trace_count = 0;
    EXPECT(dsi_pwr_enable_regulator(&group, true) == 0);
    EXPECT(group.refcount == 1);
    for (unsigned i = 0; i < group.count; ++i) {
        EXPECT(hardware[i].references == 1);
        EXPECT(hardware[i].load == rails[i].enable_load);
    }
    EXPECT(dsi_pwr_enable_regulator(&group, false) == 0);
    off_state();
}
static void nominal(unsigned count, bool sleeps, bool off_voltage)
{
    setup(count, sleeps, off_voltage);
    EXPECT(dsi_pwr_enable_regulator(&group, true) == 0);
    unsigned expected[128], n = 0;
    for (unsigned i = 0; i < count; ++i) {
        expected[n++] = i * OP_COUNT + LOAD_ON;
        if (voltage_count) expected[n++] = i * OP_COUNT + VOLT_ON;
        expected[n++] = i * OP_COUNT + ENABLE;
    }
    EXPECT(dsi_pwr_enable_regulator(&group, true) == 0);
    EXPECT(group.refcount == 2 && enable_calls() == count);
    EXPECT(dsi_pwr_enable_regulator(&group, false) == 0);
    EXPECT(group.refcount == 1);
    for (unsigned i = 0; i < count; ++i) EXPECT(hardware[i].references == 1);
    EXPECT(dsi_pwr_enable_regulator(&group, false) == 0);
    for (unsigned i = count; i-- > 0;) {
        if (off_voltage) expected[n++] = i * OP_COUNT + VOLT_OFF;
        expected[n++] = i * OP_COUNT + LOAD_OFF;
        expected[n++] = i * OP_COUNT + DISABLE;
    }
    EXPECT(trace_count == n && !memcmp(trace, expected, n * sizeof(*expected)));
    EXPECT(delays == (sleeps ? count * 4 : 0));
    off_state();
}
static void shutdown_error(unsigned count, unsigned at, enum operation op,
                           int error, bool sleeps, bool off_voltage)
{
    setup(count, sleeps, off_voltage);
    EXPECT(dsi_pwr_enable_regulator(&group, true) == 0);
    faults[at][op] = error;
    EXPECT(dsi_pwr_enable_regulator(&group, false) == error);
    unsigned calls = enable_calls();
    for (unsigned retry = 0; retry < 2; ++retry) {
        trace_count = 0;
        EXPECT(dsi_pwr_enable_regulator(&group, true) == error);
        EXPECT(enable_calls() == calls);
        EXPECT(!unbalanced);
    }
    trace_count = 0;
    EXPECT(dsi_pwr_enable_regulator(&group, false) == error);
    memset(faults, 0, sizeof(faults));
    if (!sleeps) {
        EXPECT(dsi_pwr_enable_regulator(&group, false) == 0);
        off_state();
    }
    retry_success();
}
static void startup_error(unsigned count, unsigned at, enum operation start_op,
                          int error, int cleanup_at, enum operation cleanup_op,
                          bool sleeps)
{
    setup(count, sleeps, true);
    faults[at][start_op] = error;
    if (cleanup_at >= 0) faults[cleanup_at][cleanup_op] = -ENOSPC;
    EXPECT(dsi_pwr_enable_regulator(&group, true) == error);
    EXPECT(group.refcount == 0);
    faults[at][start_op] = 0;
    if (cleanup_at >= 0) {
        unsigned calls = enable_calls();
        trace_count = 0;
        EXPECT(dsi_pwr_enable_regulator(&group, true) == -ENOSPC);
        EXPECT(enable_calls() == calls);
        EXPECT(!unbalanced);
        trace_count = 0;
        EXPECT(dsi_pwr_enable_regulator(&group, false) == -ENOSPC);
    } else off_state();
    memset(faults, 0, sizeof(faults));
    trace_count = 0;
    if (!sleeps) {
        EXPECT(dsi_pwr_enable_regulator(&group, false) == 0);
        off_state();
    }
    retry_success();
}
static void multiple_errors(unsigned count)
{
    setup(count, false, true);
    EXPECT(dsi_pwr_enable_regulator(&group, true) == 0);
    faults[count - 1][VOLT_OFF] = -ERANGE;
    faults[count - 1][LOAD_OFF] = -ENOSPC;
    faults[0][DISABLE] = -EIO;
    EXPECT(dsi_pwr_enable_regulator(&group, false) == -ERANGE);
    unsigned calls = enable_calls(); trace_count = 0;
    EXPECT(dsi_pwr_enable_regulator(&group, true) == -ERANGE);
    EXPECT(enable_calls() == calls);
    memset(faults, 0, sizeof(faults)); trace_count = 0;
    EXPECT(dsi_pwr_enable_regulator(&group, false) == 0);
    off_state(); retry_success();
}

int main(void)
{
    const int errors[] = {-EIO, -ENOMEM, -517};
    for (unsigned count = 1; count <= 8; ++count) {
        for (voltage_count = 0; voltage_count <= 1; ++voltage_count) {
            multiple_errors(count);
            for (unsigned sleeps = 0; sleeps <= 1; ++sleeps) {
                for (unsigned off_voltage = 0; off_voltage <= 1; ++off_voltage) {
                    nominal(count, sleeps, off_voltage);
                    for (unsigned at = 0; at < count; ++at)
                        for (enum operation op = VOLT_OFF; op <= DISABLE; ++op)
                            if (op != VOLT_OFF || off_voltage)
                                for (unsigned e = 0; e < sizeof(errors) / sizeof(*errors); ++e)
                                    shutdown_error(count, at, op, errors[e], sleeps, off_voltage);
                }
                for (unsigned at = 0; at < count; ++at) {
                    for (enum operation op = LOAD_ON; op <= ENABLE; ++op) {
                        if (op == VOLT_ON && !voltage_count) continue;
                        for (unsigned e = 0; e < sizeof(errors) / sizeof(*errors); ++e) {
                            startup_error(count, at, op, errors[e], -1, LOAD_OFF, sleeps);
                            for (unsigned cleanup_at = 0; cleanup_at <= at; ++cleanup_at)
                                for (enum operation clean = VOLT_OFF; clean <= DISABLE; ++clean) {
                                    if (clean == DISABLE && cleanup_at == at) continue;
                                    if (clean == VOLT_OFF && (!voltage_count || (cleanup_at == at && op == LOAD_ON))) continue;
                                    startup_error(count, at, op, errors[e], cleanup_at, clean, sleeps);
                                }
                        }
                    }
                }
            }
        }
    }
#ifdef EXPECT_OLD
    assert(defects > 1000);
#else
    assert(defects == 0);
#endif
    printf("PICO_PANEL_CLEANUP_PASS cases=%u defects=%u\n", cases, defects);
    return 0;
}
