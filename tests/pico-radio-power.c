#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
#define QCA6390_DEVICE_ID 0x1101
#define QCA6490_DEVICE_ID 0x1103
#define CNSS_VREG_PRIM 0
#define cnss_pr_dbg(...) ((void)0)
#define cnss_pr_err(...) ((void)0)
struct cnss_plat_data {
    struct { int bt_en_gpio, wlan_rffemid_gpio; } pinctrl_info;
    unsigned device_id;
    bool powered_on;
    int clk_list;
};
static int failure_stage, failure_error, sample, active_gpio;
static unsigned rails, clocks, events[32], event_count;
static void event(unsigned value) { assert(event_count < 32); events[event_count++] = value; }
static bool gpio_is_valid(int gpio) { return gpio >= 0 && gpio < 256; }
static int gpio_direction_output(int gpio, int value)
{
    assert(gpio == active_gpio && value == 0); event(3);
    return failure_stage == 1 ? failure_error : 0;
}
static int gpio_direction_input(int gpio)
{
    assert(gpio == active_gpio); event(4);
    return failure_stage == 2 ? failure_error : 0;
}
static int gpio_get_value(int gpio)
{
    assert(gpio == active_gpio); event(6);
    return failure_stage == 3 ? failure_error : sample;
}
static void msleep(unsigned delay) { assert(delay == 10); event(5); }
static int cnss_select_pinctrl_state(struct cnss_plat_data *priv, bool enabled)
{
    assert(priv && enabled); event(7);
    return failure_stage == 4 ? failure_error : 0;
}
static int cnss_vreg_on_type(struct cnss_plat_data *priv, int type)
{
    assert(priv && type == CNSS_VREG_PRIM && !rails); event(1);
    if (failure_stage == 5) return failure_error;
    rails = 1; return 0;
}
static void cnss_vreg_off_type(struct cnss_plat_data *priv, int type)
{
    assert(priv && type == CNSS_VREG_PRIM && rails && !clocks); event(9); rails = 0;
}
static int cnss_clk_on(struct cnss_plat_data *priv, int *list)
{
    assert(priv && list == &priv->clk_list && rails && !clocks); event(2);
    if (failure_stage == 6) return failure_error;
    clocks = 1; return 0;
}
static void cnss_clk_off(struct cnss_plat_data *priv, int *list)
{
    assert(priv && list == &priv->clk_list && rails && clocks); event(8); clocks = 0;
}
#include "drivers/net/wireless/cnss2/power.c"

static struct cnss_plat_data setup(unsigned chip, int gpio, int bt)
{
    rails = clocks = event_count = 0; active_gpio = gpio; cnss_wlan_rffemid = -1;
    return (struct cnss_plat_data){.device_id=chip, .pinctrl_info={bt, gpio}};
}
int main(void)
{
    assert(gpio_is_valid(160) && !gpio_is_valid(-517));
    unsigned cases = 0, defects = 0;
    const unsigned chips[] = {QCA6390_DEVICE_ID, QCA6490_DEVICE_ID};
    const int errors[] = {-EIO, -EBUSY, -517};
    for (unsigned chip = 0; chip < 2; chip++) {
        for (int bt = -1; bt <= 0; bt++) {
            for (sample = 0; sample < 2; sample++) {
                for (failure_stage = 1; failure_stage <= 3; failure_stage++) {
                    for (unsigned error = 0; error < 3; error++) {
                        failure_error = errors[error];
                        struct cnss_plat_data priv = setup(chips[chip], 160, bt);
                        int rc = cnss_power_on_device(&priv);
                        bool okay = rc == failure_error && !priv.powered_on && !rails && !clocks && cnss_wlan_rffemid == -1;
                        defects += !okay; cases++;
#ifndef EXPECT_OLD
                        assert(okay && events[event_count-2] == 8 && events[event_count-1] == 9);
                        unsigned stopped = failure_stage == 1 ? 3 : failure_stage == 2 ? 4 : 6;
                        assert(events[event_count-3] == stopped);
#endif
                    }
                }
                failure_stage = 0;
                const int invalid[] = {-EINVAL, -517, 256};
                for (unsigned i = 0; i < 3; i++) {
                    struct cnss_plat_data priv = setup(chips[chip], invalid[i], bt);
                    int rc = cnss_power_on_device(&priv);
                    int expected = invalid[i] < 0 ? invalid[i] : -EINVAL;
                    bool okay = rc == expected && !priv.powered_on && !rails && !clocks && cnss_wlan_rffemid == -1;
                    defects += !okay; cases++;
#ifndef EXPECT_OLD
                    const unsigned wanted[] = {1,2,8,9};
                    assert(okay && event_count == 4 && !memcmp(events,wanted,sizeof(wanted)));
#endif
                }
                struct cnss_plat_data priv = setup(chips[chip], 160, bt);
                const unsigned wanted[] = {1,2,3,4,5,6,7};
                assert(cnss_power_on_device(&priv) == 0 && priv.powered_on && rails && clocks && cnss_wlan_rffemid == sample);
                assert(event_count == 7 && !memcmp(events,wanted,sizeof(wanted)));
                event_count = 0;
                assert(cnss_power_on_device(&priv) == 0 && event_count == 0);
                for (failure_stage = 4; failure_stage <= 6; failure_stage++) {
                    priv = setup(chips[chip], 160, bt); failure_error = -EIO;
                    assert(cnss_power_on_device(&priv) == -EIO && !priv.powered_on && !rails && !clocks);
                }
            }
        }
    }
#ifdef EXPECT_OLD
    assert(defects == cases);
#else
    assert(defects == 0);
#endif
    printf("%u RF GPIO failures; %u ignored errors\n", cases, defects);
    return 0;
}
