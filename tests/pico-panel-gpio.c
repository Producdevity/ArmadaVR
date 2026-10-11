#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#define DSI_ERR(...) ((void)0)
#define DSI_WARN(...) ((void)0)
struct dsi_panel_reset_config { int reset_gpio, disp_en_gpio, lcd_mode_sel_gpio; };
struct dsi_panel {
    struct dsi_panel_reset_config reset_config;
    struct { int en_gpio; } bl_config;
    int panel_vio_gpio, panel_blk_gpio, panel_test_gpio;
};

static unsigned owned, requested, released, invalid_frees;
static int fail_pin, fail_error;
static bool gpio_is_valid(int pin) { return pin >= 0 && pin < 7; }
static int gpio_request(int pin, const char *name)
{
    assert(gpio_is_valid(pin) && name && !(owned & (1u << pin)));
    requested |= 1u << pin;
    if (pin == fail_pin)
        return fail_error;
    owned |= 1u << pin;
    return 0;
}
static void gpio_free(int pin)
{
    assert(gpio_is_valid(pin));
    if (!(owned & (1u << pin)))
        ++invalid_frees;
    released |= 1u << pin;
    owned &= ~(1u << pin);
}

#include "driver.inc"

static struct dsi_panel panel_for(unsigned mask)
{
    return (struct dsi_panel){
        .reset_config = {mask & 1 ? 0 : -1, mask & 2 ? 1 : -1, mask & 8 ? 3 : -1},
        .bl_config.en_gpio = mask & 4 ? 2 : -1,
        .panel_vio_gpio = mask & 16 ? 4 : -1,
        .panel_blk_gpio = mask & 32 ? 5 : -1,
        .panel_test_gpio = mask & 64 ? 6 : -1,
    };
}

int main(void)
{
    unsigned cases = 0, defects = 0;
    const int errors[] = {-517, -EBUSY, -ENOMEM};
    for (unsigned mask = 0; mask < 128; ++mask) {
        for (int pin = 0; pin < 7; ++pin) {
            if (!(mask & (1u << pin)))
                continue;
            for (unsigned error = 0; error < 3; ++error) {
                struct dsi_panel panel = panel_for(mask);
                owned = invalid_frees = 0;
                for (unsigned retry = 0; retry < 32; ++retry) {
                    requested = released = 0;
                    fail_pin = pin; fail_error = errors[error];
                    int rc = dsi_panel_gpio_request(&panel);
                    bool valid;
                    if (pin == 6) {
                        assert(rc == 0 && panel.panel_test_gpio == -1);
                        assert(owned == (mask & ~64u));
                        assert(dsi_panel_gpio_release(&panel) == 0);
                        valid = !owned && !invalid_frees;
                        panel.panel_test_gpio = 6;
                    } else {
                        assert(rc == fail_error);
                        unsigned earlier = mask & ((1u << pin) - 1);
                        valid = !owned && !invalid_frees && released == earlier &&
                            requested == (earlier | (1u << pin));
                    }
                    if (!valid)
                        ++defects;
#ifndef EXPECT_OLD
                    assert(valid);
#else
                    owned = invalid_frees = 0;
#endif
                    ++cases;
                }
                fail_pin = -1; requested = released = 0;
                assert(dsi_panel_gpio_request(&panel) == 0 && owned == mask);
                assert(dsi_panel_gpio_release(&panel) == 0 && !owned && !invalid_frees);
            }
        }
        struct dsi_panel panel = panel_for(mask);
        fail_pin = -1; requested = released = 0;
        assert(dsi_panel_gpio_request(&panel) == 0 && owned == mask);
        assert(dsi_panel_gpio_release(&panel) == 0 && !owned && !invalid_frees);
    }
    printf("%u panel GPIO failures checked; %u cleanup defects\n", cases, defects);
    return 0;
}
