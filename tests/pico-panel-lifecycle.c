#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EPROBE_DEFER 517
#define DSI_ERR(...) ((void)0)
#define pr_err(...) ((void)0)
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(pointer) (!(pointer) || IS_ERR(pointer))
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))

struct mipi_dsi_host { int unused; };
struct mipi_dsi_device { struct mipi_dsi_host *host; int channel, lanes; };
struct dsi_panel {
    int panel_lock;
    struct mipi_dsi_device mipi_device;
    struct mipi_dsi_host *host;
    void *parent, *panel_of_node, *native_mux;
    struct { bool ext_bridge_mode; } host_config;
    struct { void *pinctrl, *active, *suspend; } pinctrl;
};

enum stage { REGULATORS = 1, CHARGE, PINCTRL, ACTIVE, SUSPEND, GPIOS, MUX, BACKLIGHT };
static unsigned owned, order[8], depth, calls[8], call_count, cleanup_errors;
static enum stage fail_stage;
static int fail_error;
static bool null_result, no_mux;

static int acquire(enum stage stage, bool resource)
{
    assert(call_count < 8);
    calls[call_count++] = stage;
    if (stage == fail_stage)
        return fail_error;
    if (resource) {
        assert(!(owned & (1u << stage)) && depth < 8);
        owned |= 1u << stage;
        order[depth++] = stage;
    }
    return 0;
}

static void release(enum stage stage)
{
    if (!(owned & (1u << stage))) {
        ++cleanup_errors;
        return;
    }
    if (!depth || order[depth - 1] != (unsigned)stage)
        ++cleanup_errors;
    else
        --depth;
    owned &= ~(1u << stage);
}

static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static int dsi_panel_vreg_get(struct dsi_panel *panel)
{ assert(panel->panel_lock); return acquire(REGULATORS, true); }
static int dsi_panel_chahei_vreg_get(struct dsi_panel *panel)
{ assert(panel->panel_lock); return acquire(CHARGE, true); }
static int dsi_panel_vreg_put(struct dsi_panel *panel)
{ assert(panel->panel_lock); release(REGULATORS); return 0; }
static int dsi_panel_chahei_vreg_put(struct dsi_panel *panel)
{ assert(panel->panel_lock); release(CHARGE); return 0; }
static void *devm_pinctrl_get(void *parent)
{
    (void)parent;
    int rc = acquire(PINCTRL, true);
    return rc ? (null_result ? NULL : (void *)(intptr_t)rc) : (void *)(uintptr_t)PINCTRL;
}
static void devm_pinctrl_put(void *handle)
{ assert(handle == (void *)(uintptr_t)PINCTRL); release(PINCTRL); }
static void *pinctrl_lookup_state(void *handle, const char *name)
{
    assert(handle == (void *)(uintptr_t)PINCTRL && (owned & (1u << PINCTRL)));
    enum stage stage = !strcmp(name, "panel_active") ? ACTIVE : SUSPEND;
    int rc = acquire(stage, false);
    return rc ? (null_result ? NULL : (void *)(intptr_t)rc) : (void *)(uintptr_t)stage;
}
static int dsi_panel_pinctrl_deinit(struct dsi_panel *panel)
{
    if (!panel->host_config.ext_bridge_mode)
        devm_pinctrl_put(panel->pinctrl.pinctrl);
    return 0;
}
static int dsi_panel_gpio_request(struct dsi_panel *panel)
{ assert(panel->panel_lock); return acquire(GPIOS, true); }
static int dsi_panel_gpio_release(struct dsi_panel *panel)
{ assert(panel->panel_lock); release(GPIOS); return 0; }
static void *pico_panel_mux_get(void *node)
{
    (void)node;
    if (no_mux)
        return NULL;
    int rc = acquire(MUX, true);
    return rc ? (void *)(intptr_t)rc : (void *)(uintptr_t)MUX;
}
static void pico_panel_mux_put(void *mux)
{
    if (mux) {
        assert(mux == (void *)(uintptr_t)MUX);
        release(MUX);
    }
}
static int dsi_panel_bl_register(struct dsi_panel *panel)
{ assert(panel->panel_lock); return acquire(BACKLIGHT, true); }

#include "driver.inc"

static void dispose(struct dsi_panel *panel)
{
    mutex_lock(&panel->panel_lock);
    release(BACKLIGHT);
    pico_panel_mux_put(panel->native_mux);
    dsi_panel_gpio_release(panel);
    dsi_panel_pinctrl_deinit(panel);
    dsi_panel_chahei_vreg_put(panel);
    dsi_panel_vreg_put(panel);
    mutex_unlock(&panel->panel_lock);
}

int main(void)
{
    unsigned cases = 0, defects = 0;
    struct mipi_dsi_host host = {0};
    const int errors[] = {-EPROBE_DEFER, -ENOMEM, -EIO};
    for (unsigned variant = 0; variant < 4; ++variant) {
        no_mux = variant & 1;
        for (enum stage failure = REGULATORS; failure <= BACKLIGHT; ++failure) {
            if ((no_mux && failure == MUX) ||
                ((variant & 2) && failure >= PINCTRL && failure <= SUSPEND))
                continue;
            for (unsigned error = 0; error < 4; ++error) {
                if (error == 3 && (failure < PINCTRL || failure > SUSPEND))
                    continue;
                struct dsi_panel panel = {.host_config.ext_bridge_mode = variant & 2};
                owned = depth = cleanup_errors = 0;
                for (unsigned retry = 0; retry < 64; ++retry) {
                    call_count = 0;
                    fail_stage = failure;
                    fail_error = error == 3 ? -EINVAL : errors[error];
                    null_result = error == 3;
                    int rc = dsi_panel_drv_init(&panel, &host);
                    bool valid = rc == fail_error && !owned && !cleanup_errors && !panel.panel_lock;
                    if (failure >= PINCTRL && failure <= SUSPEND)
                        valid = valid && !panel.pinctrl.pinctrl && !panel.pinctrl.active && !panel.pinctrl.suspend;
                    if (!valid)
                        ++defects;
#ifndef EXPECT_OLD
                    assert(valid);
                    assert(call_count && calls[call_count - 1] == (unsigned)failure);
                    assert(!panel.native_mux);
#else
                    owned = depth = cleanup_errors = 0;
#endif
                    ++cases;
                }
                fail_stage = 0; call_count = 0; null_result = false;
                assert(dsi_panel_drv_init(&panel, &host) == 0);
                assert(panel.mipi_device.host == &host && panel.host == &host);
                assert(panel.mipi_device.channel == 0 && panel.mipi_device.lanes == 4);
                dispose(&panel);
                assert(!owned && !depth && !cleanup_errors);
            }
        }
    }
    struct dsi_panel panel = {0};
    assert(dsi_panel_drv_init(NULL, &host) == -EINVAL);
    assert(dsi_panel_drv_init(&panel, NULL) == -EINVAL);
    printf("%u panel initialization failures checked; %u cleanup defects\n", cases, defects);
    return 0;
}
