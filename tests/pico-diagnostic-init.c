#define main diagnostic_main
#include "../system/pico/diagnostic-init.c"
#undef main
#include <assert.h>

static void property(const char *root, const char *name, const void *data, size_t size)
{
    char path[512];
    assert(snprintf(path, sizeof(path), "%s/primary/device/of_node/%s", root, name) < (int)sizeof(path));
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0 && write(fd, data, size) == (ssize_t)size && !close(fd));
}

static void usb_selection(void)
{
    char root[] = "/tmp/pico-diagnostic-XXXXXX", path[512], name[64];
    assert(mkdtemp(root));
    const char *directories[] = {"udc", "primary", "primary/device", "primary/device/of_node"};
    for (size_t i = 0; i < sizeof(directories) / sizeof(*directories); ++i) {
        assert(snprintf(path, sizeof(path), "%s/%s", root, directories[i]) < (int)sizeof(path));
        assert(!mkdir(path, 0700));
    }
    char controllers[512];
    assert(snprintf(controllers, sizeof(controllers), "%s/udc", root) < (int)sizeof(controllers));
    assert(udc(controllers, name, sizeof(name)) == 0);
    assert(snprintf(path, sizeof(path), "%s/a800000.dwc3", controllers) < (int)sizeof(path));
    assert(!symlink("../primary", path));
    assert(udc(controllers, name, sizeof(name)) == 0);
    assert(snprintf(path, sizeof(path), "%s/%s", controllers, PRIMARY_UDC) < (int)sizeof(path));
    assert(!symlink("../primary", path));
    assert(udc(controllers, name, sizeof(name)) == -1);
    const char compatible[] = "snps,dwc3";
    unsigned char reg[] = {0x0a, 0x60, 0, 0, 0, 0, 0xd9, 0x3c};
    unsigned char core[] = {0, 0, 0, 0};
    property(root, "compatible", compatible, sizeof(compatible));
    property(root, "reg", reg, sizeof(reg));
    property(root, "usb-core-id", core, sizeof(core));
    assert(udc(controllers, name, sizeof(name)) == 1 && !strcmp(name, PRIMARY_UDC));
    assert(udc(controllers, name, 4) == -1);
    for (size_t i = 0; i < sizeof(reg); ++i) {
        reg[i] ^= 1;
        property(root, "reg", reg, sizeof(reg));
        assert(udc(controllers, name, sizeof(name)) == -1 && !name[0]);
        reg[i] ^= 1;
    }
    property(root, "reg", reg, sizeof(reg) - 1);
    assert(udc(controllers, name, sizeof(name)) == -1);
    property(root, "reg", reg, sizeof(reg));
    core[3] = 1;
    property(root, "usb-core-id", core, sizeof(core));
    assert(udc(controllers, name, sizeof(name)) == -1);
    core[3] = 0;
    property(root, "usb-core-id", core, sizeof(core));
    property(root, "compatible", "snps,dwc3-fake", sizeof("snps,dwc3-fake"));
    assert(udc(controllers, name, sizeof(name)) == -1);
    property(root, "compatible", compatible, sizeof(compatible));
    assert(udc(controllers, name, sizeof(name)) == 1);
    assert(snprintf(path, sizeof(path), "%s/state", root) < (int)sizeof(path));
    assert(configured(path) == -1);
    const char *states[] = {"not attached\n", "powered\n", "addressed\n", "suspended\n",
                            "configured", "configured\nextra", "configured\n"};
    for (size_t i = 0; i < sizeof(states) / sizeof(*states); ++i) {
        int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
        assert(fd >= 0 && write(fd, states[i], strlen(states[i])) == (ssize_t)strlen(states[i]) && !close(fd));
        assert(configured(path) == (i == sizeof(states) / sizeof(*states) - 1));
    }
    assert(!unlink(path));
    const char *files[] = {"primary/device/of_node/compatible", "primary/device/of_node/reg",
        "primary/device/of_node/usb-core-id", "udc/" PRIMARY_UDC, "udc/a800000.dwc3"};
    for (size_t i = 0; i < sizeof(files) / sizeof(*files); ++i) {
        assert(snprintf(path, sizeof(path), "%s/%s", root, files[i]) < (int)sizeof(path));
        assert(!unlink(path));
    }
    for (size_t i = sizeof(directories) / sizeof(*directories); i > 0; --i) {
        assert(snprintf(path, sizeof(path), "%s/%s", root, directories[i - 1]) < (int)sizeof(path));
        assert(!rmdir(path));
    }
    assert(!rmdir(root));
}

static void display_guard(void)
{
    char root[] = "/tmp/pico-display-XXXXXX", path[512];
    assert(mkdtemp(root));
    const char *directories[] = {"module", "module/msm_drm", "module/msm_drm/parameters",
        "bus", "bus/platform", "bus/platform/drivers"};
    for (size_t i = 0; i < sizeof(directories) / sizeof(*directories); i++) {
        assert(snprintf(path, sizeof(path), "%s/%s", root, directories[i]) < (int)sizeof(path));
        assert(!mkdir(path, 0700));
    }
    assert(!display_disabled(root));
    assert(snprintf(path, sizeof(path), "%s/module/msm_drm/parameters/modeset", root) < (int)sizeof(path));
    const char *values[] = {"Y\n", "0\n", "N", "N\nextra", "", "N\n"};
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); i++) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        assert(fd >= 0 && write(fd, values[i], strlen(values[i])) == (ssize_t)strlen(values[i]) && !close(fd));
        assert(display_disabled(root) == (i == sizeof(values) / sizeof(*values) - 1));
    }
    char driver[512];
    assert(snprintf(driver, sizeof(driver), "%s/bus/platform/drivers/msm_drm", root) < (int)sizeof(driver));
    assert(!mkdir(driver, 0700));
    assert(!display_disabled(root));
    assert(!rmdir(driver));
    assert(!symlink("/missing", driver));
    assert(!display_disabled(root));
    assert(!unlink(driver));
    assert(display_disabled(root));
    assert(!unlink(path));
    for (size_t i = sizeof(directories) / sizeof(*directories); i > 0; i--) {
        assert(snprintf(path, sizeof(path), "%s/%s", root, directories[i - 1]) < (int)sizeof(path));
        assert(!rmdir(path));
    }
    assert(!rmdir(root));
}

int main(void)
{
    assert(getpid() != 1);
    assert(diagnostic_main() == 2);
    const char *command = "rdinit=/init armada.pico=diagnostic androidboot.serialno=test";
    assert(token(command, "armada.pico=diagnostic"));
    assert(token(command, "androidboot.serialno=test"));
    assert(!token(command, "androidboot.serialno=tes"));
    assert(!token(command, "armada.pico=diagnostic-test"));
    assert(!token("armada.pico=diagnostic armada.pico=diagnostic", "armada.pico=diagnostic"));
    assert(!token("armada.pico=diagnostic armada.pico=wrong", "armada.pico=diagnostic"));
    assert(!token("badarmada.pico=diagnostic", "armada.pico=diagnostic"));
    assert(token("\tarmada.pico=diagnostic\n", "armada.pico=diagnostic"));
    assert(!token("", "armada.pico=diagnostic"));
    assert(token("msm_drm.modeset=0", "msm_drm.modeset=0"));
    assert(!token("msm_drm.modeset=0 msm_drm.modeset=1", "msm_drm.modeset=0"));
    assert(!token("msm_drm.modeset=0 msm_drm.modeset=0", "msm_drm.modeset=0"));
    display_guard();
    usb_selection();
    for (unsigned int i = 0; i < 2000; ++i) record("%0100u\n", i);
    assert(used == sizeof(report) - 1 && report[used] == 0);
    puts("PICO_DIAGNOSTIC_INIT_PASS");
    return 0;
}
