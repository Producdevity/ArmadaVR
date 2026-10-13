#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error Assertions are required
#endif

struct device { int usage; };
struct base { int id; };
struct drm_display_mode { struct base base; const char *name; int type, flags; };
struct crtc_state { int encoder_mask; bool active; struct drm_display_mode adjusted_mode; };
struct drm_crtc { struct base base; struct crtc_state *state; struct drm_display_mode mode; };
struct drm_encoder { const char *name; struct base base; struct drm_crtc *crtc; };
struct bridge { struct { struct drm_encoder *encoder; } base; };
struct power_info { int unused; };
struct panel {
    const char *name;
    struct power_info power_info;
#ifdef TEST_SPLASH_MUX
    void *native_mux;
    int panel_lock;
#endif
};
struct msm_drm_private { struct drm_crtc *crtcs[2]; };
struct drm_device {
    struct device *dev;
    struct msm_drm_private *dev_private;
    struct { int mutex, max_width, max_height; } mode_config;
};
struct dsi_display {
    const char *name;
    struct drm_device *drm_dev;
    struct panel *panel;
    struct bridge *bridge;
    int display_lock;
    bool is_cont_splash_enabled;
    void *clk_mngr, *dsi_clk_handle;
};
struct msm_display_info { bool is_connected; int display_type; };
struct drm_connector;
struct connector_funcs { void (*fill_modes)(struct drm_connector *, int, int); };
struct drm_connector {
    int encoder_ids[1];
    struct connector_funcs *funcs;
    struct drm_display_mode modes;
};
struct sde_connector {
    struct drm_connector base;
    struct { int (*cont_splash_config)(void *); } ops;
    void *display;
};
struct drm_connector_list_iter { int unused; };
struct sde_splash_display { bool cont_splash_enabled; struct drm_encoder *encoder; };
struct msm_kms { int unused; };
struct sde_kms {
    struct msm_kms base;
    struct drm_device *dev;
    struct { int num_splash_regions, num_splash_displays; struct sde_splash_display splash_display[2]; } splash_data;
    int dsi_display_count;
    void *dsi_displays[2];
};

static int pm_error, clk_on_error, panel_error, clk_off_error, pm_put_error;
static int lock_count, refs_start, regulator_calls, handoffs, brightness, planes, logs;
static int clk_on_calls, clk_off_calls, pm_get_calls, pm_put_calls, noidle_calls;
static bool manager, isr;
static struct sde_connector connectors[2];
static int iterator;
static unsigned cases, defects;

#define DSI_ALL_CLKS 3
#define DSI_CLK_ON 1
#define DSI_CLK_OFF 0
#define DSI_ERR(...) do { logs++; } while (0)
#define SDE_ERROR(...) do { logs++; } while (0)
#define SDE_DEBUG(...) ((void)0)
#define DRM_INFO(...) ((void)0)
#define list_first_entry(ptr, type, member) ((type *)(ptr))
#define drm_for_each_connector_iter(conn, iter) \
    for (iterator = 0; iterator < 2 && ((conn) = &connectors[iterator].base); iterator++)
#define to_sde_connector(ptr) ((struct sde_connector *)(ptr))
#define to_sde_kms(ptr) ((struct sde_kms *)(ptr))

static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; lock_count++; }
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; lock_count--; }
static int pm_runtime_get_sync(struct device *dev) { pm_get_calls++; dev->usage++; return pm_error; }
static int pm_runtime_put_sync(struct device *dev) { pm_put_calls++; assert(dev->usage > 0); dev->usage--; return pm_put_error; }
static inline __attribute__((unused)) void pm_runtime_put_noidle(struct device *dev) { noidle_calls++; assert(dev->usage > 0); dev->usage--; }
static void dsi_display_clk_mngr_update_splash_status(void *unused, bool on) { (void)unused; manager = on; }
static void dsi_display_ctrl_isr_configure(struct dsi_display *unused, bool on) { (void)unused; isr = on; }
static int dsi_display_clk_ctrl(void *unused, int clks, int on)
{
    (void)unused; assert(clks == DSI_ALL_CLKS);
    if (on) { clk_on_calls++; return clk_on_error; }
    clk_off_calls++; return clk_off_error;
}
static int dsi_pwr_enable_regulator(struct power_info *unused, bool on) { (void)unused; assert(on); regulator_calls++; return panel_error; }
static void dsi_config_host_engine_state_for_cont_splash(struct dsi_display *unused) { (void)unused; handoffs++; }
static void dsi_panel_bl_handoff(struct panel *unused) { (void)unused; assert(!lock_count); brightness++; }
#ifdef TEST_SPLASH_MUX
static int mux_error, mux_calls;
static inline __attribute__((unused)) int pico_panel_mux_prepare(void *mux)
{
    (void)mux;
    assert(lock_count == 2);
    mux_calls++;
    return mux_error;
}
#endif
#include "display.inc"

static int dsi_display_get_info(void *unused, struct msm_display_info *info, void *display)
{ (void)unused; (void)display; info->is_connected = true; return 0; }
static void drm_connector_list_iter_begin(struct drm_device *dev, struct drm_connector_list_iter *iter) { (void)dev; (void)iter; }
static void drm_connector_list_iter_end(struct drm_connector_list_iter *iter) { (void)iter; }
static int drm_encoder_index(struct drm_encoder *encoder) { return encoder->base.id; }
static int drm_atomic_set_mode_for_crtc(struct crtc_state *state, struct drm_display_mode *mode) { (void)state; (void)mode; return 0; }
static void drm_mode_copy(struct drm_display_mode *to, struct drm_display_mode *from) { *to = *from; }
static void sde_encoder_update_caps_for_cont_splash(struct drm_encoder *encoder, struct sde_splash_display *splash, bool on)
{ (void)encoder; (void)splash; assert(on); }
static void sde_crtc_update_cont_splash_settings(struct drm_crtc *crtc) { (void)crtc; }
static int _sde_kms_update_planes_for_cont_splash(struct sde_kms *kms, struct sde_splash_display *splash, struct drm_crtc *crtc)
{ (void)kms; (void)splash; (void)crtc; planes++; return 0; }
static void fill_modes(struct drm_connector *conn, int w, int h) { (void)conn; (void)w; (void)h; }
#include "kms.inc"

static void check(bool good) { if (!good) defects++; }

static void run(int pm, int clock_on, int panel, int clock_off, int put, int refs, bool kms_path)
{
    struct device dev = {.usage = refs};
    struct crtc_state states[2] = {0};
    struct drm_crtc crtcs[2] = {{.state = &states[0]}, {.state = &states[1]}};
    struct drm_encoder encoders[2] = {{.base.id = 1}, {.base.id = 2}};
    struct bridge bridges[2] = {{.base.encoder = &encoders[0]}, {.base.encoder = &encoders[1]}};
    struct msm_drm_private priv = {.crtcs = {&crtcs[0], &crtcs[1]}};
    struct drm_device drm = {.dev = &dev, .dev_private = &priv};
    struct panel panels[2] = {0};
    struct dsi_display displays[2] = {
        {.drm_dev = &drm, .panel = &panels[0], .bridge = &bridges[0]},
        {.drm_dev = &drm, .panel = &panels[1], .bridge = &bridges[1]},
    };
    struct sde_kms kms = {.dev = &drm, .dsi_display_count = 2, .dsi_displays = {&displays[0], &displays[1]},
        .splash_data = {.num_splash_regions = 2, .num_splash_displays = 2,
            .splash_display = {{.cont_splash_enabled = true}, {.cont_splash_enabled = true}}}};
    struct connector_funcs funcs = {.fill_modes = fill_modes};
    for (int i = 0; i < 2; i++) {
        connectors[i] = (struct sde_connector){.base = {.encoder_ids = {i + 1}, .funcs = &funcs},
            .ops.cont_splash_config = dsi_display_cont_splash_config, .display = &displays[i]};
    }
    pm_error = pm; clk_on_error = clock_on; panel_error = panel; clk_off_error = clock_off; pm_put_error = put;
    refs_start = refs; lock_count = regulator_calls = handoffs = brightness = planes = logs = 0;
    clk_on_calls = clk_off_calls = pm_get_calls = pm_put_calls = noidle_calls = 0;
    manager = isr = false;
    int rc = kms_path ? sde_kms_cont_splash_config(&kms.base) : dsi_display_cont_splash_config(&displays[0]);
    int expected = pm < 0 ? pm : clock_on ? clock_on : panel;
    int count = kms_path && !expected ? 2 : 1;
    check(rc == expected);
    check(lock_count == 0);
    check(dev.usage == refs_start + (expected ? 0 : count));
    check(pm_get_calls == count);
    check(noidle_calls == (pm < 0 ? count : 0));
    check(pm_put_calls == (pm >= 0 && expected ? count : 0));
    check(regulator_calls == (pm >= 0 && !clock_on ? count : 0));
    check(clk_on_calls == (pm >= 0 ? count : 0));
    check(clk_off_calls == (pm >= 0 && !clock_on && panel ? count : 0));
    check(handoffs == (expected ? 0 : count));
    check(brightness == handoffs);
    check(planes == (kms_path && !expected ? count : 0));
    check(manager == !expected && isr == !expected);
    check(displays[0].is_cont_splash_enabled == !expected);
    if (kms_path) check(displays[1].is_cont_splash_enabled == !expected);
    cases++;
}

int main(void)
{
    assert(dsi_display_cont_splash_config(NULL) == -EINVAL);
    assert(sde_kms_cont_splash_config(NULL) == -EINVAL);
    const int errors[] = {0, -EIO, -ENOMEM, -ETIMEDOUT};
    for (int path = 0; path < 2; path++)
        for (int refs = 0; refs <= 4; refs += 2)
            for (int pm = 0; pm < 5; pm++)
                for (int on = 0; on < 4; on++)
                    for (int rail = 0; rail < 4; rail++)
                        for (int off = 0; off < 4; off++)
                            for (int put = 0; put < 4; put++)
                                run(pm == 4 ? 1 : errors[pm], errors[on], errors[rail], errors[off], errors[put], refs, path);
#ifdef EXPECT_OLD
    assert(defects > 0);
#else
    assert(defects == 0);
#endif
    printf("PICO_SPLASH_PASS cases=%u defects=%u\n", cases, defects);
    return 0;
}
