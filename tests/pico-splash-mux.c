#define TEST_SPLASH_MUX
#define main splash_error_main
#include "pico-splash.c"
#undef main

int main(void)
{
    const int errors[] = {-EBUSY, -ENODEV, -EIO, -EINVAL, -517};
    for (int refs = 0; refs <= 8; refs++) {
        for (int active = 0; active < 2; active++) {
            for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
                struct device dev = {.usage = refs};
                struct drm_device drm = {.dev = &dev};
                struct panel panel = {.native_mux = &dev};
                struct dsi_display display = {.drm_dev = &drm, .panel = &panel};
                lock_count = pm_put_calls = pm_get_calls = mux_calls = 0;
                clk_on_calls = clk_off_calls = regulator_calls = handoffs = brightness = 0;
                manager = isr = false;
                pm_error = active; clk_on_error = panel_error = clk_off_error = pm_put_error = 0;
                mux_error = errors[i];
                int rc = dsi_display_cont_splash_config(&display);
                check(rc == mux_error);
                check(mux_calls == 1);
                check(dev.usage == refs);
                check(pm_get_calls == 1 && pm_put_calls == 1);
                check(clk_on_calls == 0 && clk_off_calls == 0);
                check(regulator_calls == 0);
                check(handoffs == 0 && brightness == 0);
                check(!manager && !isr && !display.is_cont_splash_enabled);
                check(lock_count == 0 && panel.panel_lock == 0);
                cases++;
            }
        }
    }
    unsigned rejected_cases = cases, rejected_defects = defects;
    cases = defects = 0;
    mux_error = 0;
    assert(splash_error_main() == 0);
    assert(cases == 7680 && defects == 0);
#ifdef EXPECT_MUX_OLD
    assert(rejected_defects == 720);
#else
    assert(rejected_defects == 0);
#endif
    printf("PICO_SPLASH_MUX_PASS refusals=%u defects=%u success_regressions=%u\n",
           rejected_cases, rejected_defects, cases);
    return 0;
}
