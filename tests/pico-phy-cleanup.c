#define main panel_cleanup_regression
#include "pico-panel-cleanup.c"
#undef main

#define DSI_PHY_ERR(...) ((void)0)
#define DSI_PHY_ENGINE_OFF 0
struct msm_dsi_phy {
    unsigned phy_lock, dsi_phy_state;
    bool power_state, regulator_required;
    struct { struct dsi_regulator_info digital, phy_pwr; } pwr_info;
};
static void mutex_lock(unsigned *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(unsigned *lock) { assert(*lock); *lock = 0; }
#include "phy.inc"

static struct msm_dsi_phy phy_setup(void)
{
    setup(2, false, true);
    struct msm_dsi_phy phy = {.regulator_required = true};
    phy.pwr_info.digital.vregs = rails; phy.pwr_info.digital.count = 1;
    phy.pwr_info.phy_pwr.vregs = rails + 1; phy.pwr_info.phy_pwr.count = 1;
    return phy;
}
static void check_off(struct msm_dsi_phy *phy)
{
    EXPECT(!phy->phy_lock && !phy->power_state && !unbalanced);
    EXPECT(!phy->pwr_info.digital.refcount && !phy->pwr_info.phy_pwr.refcount);
    EXPECT(!phy->pwr_info.digital.cleanup_pending && !phy->pwr_info.phy_pwr.cleanup_pending);
    for (unsigned i = 0; i < 2; ++i) {
        EXPECT(hardware[i].references == 0);
        EXPECT(hardware[i].load == rails[i].disable_load);
    }
}
static void full_retry(struct msm_dsi_phy *phy)
{
    trace_count = 0;
    EXPECT(dsi_phy_set_power_state(phy, true) == 0 && phy->power_state);
    EXPECT(hardware[0].references == 1 && hardware[1].references == 1);
    EXPECT(dsi_phy_set_power_state(phy, false) == 0);
    check_off(phy);
}
int main(void)
{
    const int errors[] = {-EIO, -ENOMEM, -517};
    voltage_count = 1;
    for (unsigned e = 0; e < sizeof(errors) / sizeof(*errors); ++e) {
        for (unsigned at = 0; at < 2; ++at) {
            for (enum operation op = VOLT_OFF; op <= DISABLE; ++op) {
                struct msm_dsi_phy phy = phy_setup();
                EXPECT(dsi_phy_set_power_state(&phy, true) == 0);
                faults[at][op] = errors[e];
                EXPECT(dsi_phy_set_power_state(&phy, false) == errors[e]);
                unsigned before = enable_calls();
                EXPECT(dsi_phy_set_power_state(&phy, true) == -EBUSY);
                EXPECT(enable_calls() == before);
                EXPECT(dsi_phy_set_power_state(&phy, false) == errors[e]);
                memset(faults, 0, sizeof(faults));
                EXPECT(dsi_phy_set_power_state(&phy, false) == 0);
                check_off(&phy); full_retry(&phy);
            }
        }
        for (unsigned at = 0; at < 2; ++at) {
            for (enum operation op = LOAD_ON; op <= ENABLE; ++op) {
                struct msm_dsi_phy phy = phy_setup();
                faults[at][op] = errors[e];
                faults[0][LOAD_OFF] = -ENOSPC;
                EXPECT(dsi_phy_set_power_state(&phy, true) == errors[e]);
                faults[at][op] = 0;
                unsigned before = enable_calls();
                EXPECT(dsi_phy_set_power_state(&phy, true) == -EBUSY);
                EXPECT(enable_calls() == before);
                EXPECT(dsi_phy_set_power_state(&phy, false) == -ENOSPC);
                memset(faults, 0, sizeof(faults));
                EXPECT(dsi_phy_set_power_state(&phy, false) == 0);
                check_off(&phy); full_retry(&phy);
            }
        }
    }
    for (unsigned changed = 0; changed < 2; ++changed) {
        struct msm_dsi_phy phy = phy_setup();
        EXPECT(dsi_phy_set_power_state(&phy, true) == 0);
        faults[1][DISABLE] = -EIO;
        EXPECT(dsi_phy_set_power_state(&phy, false) == -EIO);
        if (changed) phy.regulator_required = false;
        else phy.dsi_phy_state = 1;
        EXPECT(dsi_phy_set_power_state(&phy, true) == -EBUSY);
        memset(faults, 0, sizeof(faults));
        EXPECT(dsi_phy_set_power_state(&phy, false) == 0);
        check_off(&phy);
        phy.regulator_required = true; phy.dsi_phy_state = DSI_PHY_ENGINE_OFF;
        full_retry(&phy);
    }
    struct msm_dsi_phy phy = phy_setup();
    EXPECT(dsi_phy_set_power_state(&phy, false) == 0);
    EXPECT(dsi_phy_set_power_state(&phy, true) == 0);
    unsigned before = enable_calls();
    EXPECT(dsi_phy_set_power_state(&phy, true) == 0 && enable_calls() == before);
    EXPECT(dsi_phy_set_power_state(&phy, false) == 0);
    check_off(&phy);
#ifdef EXPECT_PHY_OLD
    assert(defects > 0);
#else
    assert(defects == 0);
#endif
    printf("PICO_PHY_CLEANUP_PASS cases=%u defects=%u\n", cases, defects);
}
