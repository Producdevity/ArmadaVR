#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

struct panel { bool ulps_suspend_enabled; };
struct dsi_display {
    int display_lock;
    bool poms_pending;
    struct panel *panel;
    void *dsi_clk_handle;
};
enum { WAKE, PANEL, HOST, LINK, CTRL, PHY, CORE, POST, ISR, UNREGISTER };
enum { DSI_LINK_CLK, DSI_CORE_CLK, DSI_CLK_OFF };
static int errors[8], sequence[10], count, locks;
static unsigned cases, defects;
#define DSI_ERR(...) ((void)0)
#define SDE_EVT32(...) ((void)0)

static void mutex_lock(int *lock) { assert(!*lock && !locks); *lock = 1; locks++; }
static void mutex_unlock(int *lock) { assert(*lock == 1 && locks == 1); *lock = 0; locks--; }
static int step(int id)
{
    assert(count < 10);
    assert(locks == (id != UNREGISTER));
    sequence[count++] = id;
    return id < 8 ? errors[id] : 0;
}
static int dsi_display_wake_up(struct dsi_display *d) { (void)d; return step(WAKE); }
static int dsi_panel_unprepare(struct panel *p) { (void)p; return step(PANEL); }
static int dsi_display_ctrl_host_disable(struct dsi_display *d) { (void)d; return step(HOST); }
static int dsi_display_clk_ctrl(void *h, int clk, int state)
{
    assert(h == errors && state == DSI_CLK_OFF);
    return step(clk == DSI_LINK_CLK ? LINK : CORE);
}
static int dsi_display_ctrl_deinit(struct dsi_display *d) { (void)d; return step(CTRL); }
static int dsi_display_phy_disable(struct dsi_display *d) { (void)d; return step(PHY); }
static int dsi_panel_post_unprepare(struct panel *p) { (void)p; return step(POST); }
static void dsi_display_ctrl_isr_configure(struct dsi_display *d, bool on)
{ (void)d; assert(!on); step(ISR); }
static void dsi_display_unregister_error_handler(struct dsi_display *d) { (void)d; step(UNREGISTER); }

#include "display.inc"

int main(void)
{
    const int values[] = {0, -EIO, -ETIMEDOUT};
    assert(dsi_display_unprepare(NULL) == -EINVAL && count == 0 && locks == 0);
    for (int poms = 0; poms < 2; poms++) {
        for (int ulps = 0; ulps < 2; ulps++) {
            for (unsigned permutation = 0; permutation < 6561; permutation++) {
                unsigned value = permutation;
                for (int i = 0; i < 8; i++) { errors[i] = values[value % 3]; value /= 3; }
                struct panel panel = {.ulps_suspend_enabled = ulps};
                struct dsi_display display = {.panel = &panel, .poms_pending = poms, .dsi_clk_handle = errors};
                int expected[10], length = 0, error = 0;
                for (int i = 0; i < 8; i++) {
                    if ((poms && (i == PANEL || i == POST)) || (ulps && i == PHY)) continue;
                    if (!error) error = errors[i];
                    expected[length++] = i;
                    if (i == CORE) expected[length++] = ISR;
                }
                expected[length++] = UNREGISTER;
                count = 0;
                int rc = dsi_display_unprepare(&display);
                if (rc != error) defects++;
                assert(count == length && memcmp(expected, sequence, length * sizeof(int)) == 0);
                assert(locks == 0 && display.display_lock == 0);
                assert(display.poms_pending == poms && panel.ulps_suspend_enabled == ulps);
                cases++;
            }
        }
    }
#ifdef EXPECT_OLD
    assert(defects > 0);
#else
    assert(defects == 0);
#endif
    printf("PICO_DISPLAY_SHUTDOWN_PASS cases=%u defects=%u\n", cases, defects);
    return 0;
}
