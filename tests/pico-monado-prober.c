#include "pico-monado-prober.h"
#include "pico-ssc-session.h"
#include "ssc-clock-linux.h"
#include <xrt/xrt_prober.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned opened, closed, checks;
static bool refuse;
static struct xrt_device controllers[2];
static const char *prefix = "{\"status\":\"passed\",\"read_only\":true,\"device\":\"PICOA7H10\","
    "\"fingerprint\":\"Pico/A7H10/PICOA7H10:10/5.8.4.0/smartcm.1696861506:user/test-keys\","
    "\"controller\":";

bool ssc_clock_calibrate_monotonic(struct ssc_clock *clock, unsigned attempts, int64_t max_span_ns)
{
    assert(clock && attempts == 16 && max_span_ns == 1000000);
    return true;
}

bool pico_ssc_session_create(const struct pico_ssc_session_config *config,
                             struct pico_ssc_session **session,
                             struct xrt_device **left, struct xrt_device **right)
{
    opened++;
    assert(!*session && !*left && !*right);
    assert(config->mcu_type == 1 && config->radio_type == 2 && config->touch_supported);
    assert(!config->left && !config->right);
    assert(config->max_age_ns == 100000000 && config->timeout_ns == 500000000);
    struct ssc_clock clock;
    assert(config->calibrate(config->clock_data, &clock));
    if (refuse)
        return false;
    *session = calloc(1, 1);
    assert(*session);
    *left = &controllers[0];
    *right = &controllers[1];
    return true;
}

void pico_ssc_session_destroy(struct pico_ssc_session **session)
{
    if (*session) {
        closed++;
        free(*session);
        *session = NULL;
    }
}

static void write_file(const char *path, const char *body, const char *tail)
{
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(prefix, file) >= 0 && fputs(body, file) >= 0 && fputs(tail, file) >= 0);
    assert(!fclose(file));
}

static void probe(unsigned expected)
{
    struct xrt_auto_prober *prober = pico_ssc_create_auto_prober();
    assert(prober);
    struct xrt_device *devices[XRT_MAX_DEVICES_PER_PROBE] = {NULL};
    unsigned before = opened;
    assert(prober->lelo_dallas_autoprobe(prober, NULL, true, NULL, devices) == (int)expected);
    if (expected) {
        assert(devices[0] == &controllers[0] && devices[1] == &controllers[1]);
        assert(opened == before + 1);
        struct xrt_device *again[XRT_MAX_DEVICES_PER_PROBE] = {NULL};
        assert(!prober->lelo_dallas_autoprobe(prober, NULL, false, NULL, again));
        assert(!again[0] && !again[1] && opened == before + 1);
    } else {
        assert(!devices[0] && !devices[1]);
        assert(opened == before + (refuse ? 1 : 0));
    }
    for (unsigned i = 2; i < XRT_MAX_DEVICES_PER_PROBE; i++)
        assert(!devices[i]);
    prober->destroy(prober);
    checks++;
}

int main(void)
{
    char directory[] = "/tmp/armada-pico-prober-XXXXXX";
    assert(mkdtemp(directory));
    char path[256];
    snprintf(path, sizeof(path), "%s/config.json", directory);
    const char *valid = "{\"mcu_type\":1,\"radio_type\":2,\"touch_supported\":true}";
    write_file(path, valid, "}");
    unsetenv("PICO_SSC_CONFIG");
    unsetenv("PICO_SSC_COUNTER");
    probe(0);
    assert(!setenv("PICO_SSC_CONFIG", path, 1));
    probe(0);
    assert(!setenv("PICO_SSC_COUNTER", "true", 1));
    probe(0);
    assert(!setenv("PICO_SSC_COUNTER", "1", 1));
    for (unsigned i = 0; i < 100; i++)
        probe(2);
    assert(opened == 100 && closed == 100);
    refuse = true;
    probe(0);
    refuse = false;
    const char *bad[] = {
        "null", "[]", "{}", "{\"mcu_type\":0,\"radio_type\":2,\"touch_supported\":true}",
        "{\"mcu_type\":1.5,\"radio_type\":2,\"touch_supported\":true}",
        "{\"mcu_type\":1,\"radio_type\":3,\"touch_supported\":true}",
        "{\"mcu_type\":1,\"radio_type\":2,\"touch_supported\":1}",
        "{\"mcu_type\":1,\"mcu_type\":2,\"radio_type\":2,\"touch_supported\":true}",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        write_file(path, bad[i], "}");
        probe(0);
    }
    const char *tails[] = {
        "} garbage", ",\"controller\":null}", ",\"status\":\"failed\"}",
        ",\"device\":\"different\"}", ",\"fingerprint\":\"different\"}",
        ",\"read_only\":false}",
    };
    for (unsigned i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
        write_file(path, valid, tails[i]);
        probe(0);
    }
    write_file(path, valid, "}");
    FILE *file = fopen(path, "a");
    assert(file && fputc(0, file) != EOF && !fclose(file));
    probe(0);
    file = fopen(path, "w");
    assert(file && !ftruncate(fileno(file), 65537) && !fclose(file));
    probe(0);
    assert(!unlink(path));
    probe(0);
    assert(!mkfifo(path, 0600));
    probe(0);
    assert(!unlink(path));
    assert(!setenv("PICO_SSC_CONFIG", directory, 1));
    probe(0);
    assert(!rmdir(directory));
    assert(opened == 101 && closed == 100);
    printf("PICO_MONADO_PROBER_PASS checks=%u sessions=100\n", checks);
    return 0;
}
