#include "pico-monado-prober.h"
#include "pico-ssc-session.h"
#include "ssc-clock-linux.h"

#include <cjson/cJSON.h>
#include <xrt/xrt_prober.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct pico_prober {
    struct xrt_auto_prober base;
    struct pico_ssc_session *session;
    bool attempted;
};

static const cJSON *member(const cJSON *object, const char *name)
{
    if (!cJSON_IsObject(object))
        return NULL;
    const cJSON *found = NULL;
    for (const cJSON *item = object->child; item; item = item->next) {
        if (!strcmp(item->string, name)) {
            if (found)
                return NULL;
            found = item;
        }
    }
    return found;
}

static bool equals(const cJSON *item, const char *value)
{
    return cJSON_IsString(item) && !strcmp(item->valuestring, value);
}

static bool calibrate(void *data, struct ssc_clock *clock)
{
    (void)data;
    return ssc_clock_calibrate_monotonic(clock, 16, 1000000);
}

static bool configuration(const char *path, struct pico_ssc_session_config *config)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st;
    char *buffer = NULL;
    bool valid = false;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 65536)
        goto close_file;
    buffer = malloc((size_t)st.st_size + 1);
    if (!buffer)
        goto close_file;
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t count = read(fd, buffer + used, (size_t)st.st_size - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            goto close_file;
        used += (size_t)count;
    }
    char extra;
    if (read(fd, &extra, 1) != 0 || memchr(buffer, 0, used))
        goto close_file;
    buffer[used] = 0;
    cJSON *json = cJSON_ParseWithLengthOpts(buffer, used + 1, NULL, true);
    const cJSON *controller = member(json, "controller");
    const cJSON *mcu = member(controller, "mcu_type");
    const cJSON *radio = member(controller, "radio_type");
    const cJSON *touch = member(controller, "touch_supported");
    valid = equals(member(json, "status"), "passed") && cJSON_IsTrue(member(json, "read_only")) &&
            equals(member(json, "device"), "PICOA7H10") &&
            equals(member(json, "fingerprint"),
                   "Pico/A7H10/PICOA7H10:10/5.8.4.0/smartcm.1696861506:user/test-keys") &&
            cJSON_IsNumber(mcu) && (mcu->valuedouble == 1 || mcu->valuedouble == 2) &&
            cJSON_IsNumber(radio) && (radio->valuedouble == 1 || radio->valuedouble == 2) &&
            cJSON_IsBool(touch);
    if (valid) {
        *config = (struct pico_ssc_session_config){
            .calibrate = calibrate, .max_age_ns = 100000000, .timeout_ns = 500000000,
            .mcu_type = (unsigned)mcu->valuedouble, .radio_type = (unsigned)radio->valuedouble,
            .touch_supported = cJSON_IsTrue(touch),
        };
    }
    cJSON_Delete(json);
close_file:
    free(buffer);
    close(fd);
    return valid;
}

static int autoprobe(struct xrt_auto_prober *base, cJSON *attached_data, bool no_hmds,
                     struct xrt_prober *prober, struct xrt_device **devices)
{
    (void)attached_data;
    (void)no_hmds;
    (void)prober;
    struct pico_prober *pico = (struct pico_prober *)base;
    const char *path = getenv("PICO_SSC_CONFIG");
    const char *counter = getenv("PICO_SSC_COUNTER");
    if (pico->attempted || !path || !*path || !counter || strcmp(counter, "1") ||
        !devices || devices[0] || devices[1])
        return 0;
    pico->attempted = true;
    struct pico_ssc_session_config config;
    if (!configuration(path, &config)) {
        fprintf(stderr, "Pico SSC: invalid stock controller configuration\n");
        return 0;
    }
    return pico_ssc_session_create(&config, &pico->session, &devices[0], &devices[1]) ? 2 : 0;
}

static void destroy(struct xrt_auto_prober *base)
{
    struct pico_prober *pico = (struct pico_prober *)base;
    pico_ssc_session_destroy(&pico->session);
    free(pico);
}

struct xrt_auto_prober *pico_ssc_create_auto_prober(void)
{
    struct pico_prober *pico = calloc(1, sizeof(*pico));
    if (!pico)
        return NULL;
    pico->base.name = "Pico SSC";
    pico->base.lelo_dallas_autoprobe = autoprobe;
    pico->base.destroy = destroy;
    return &pico->base;
}
