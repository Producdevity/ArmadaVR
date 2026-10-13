#include "pico-hmd-prober.h"
#include "pico-hmd-config.h"
#include <xrt/xrt_prober.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct hmd_prober { struct xrt_auto_prober base; bool attempted; };

static int autoprobe(struct xrt_auto_prober *base, cJSON *attached, bool no_hmds,
                     struct xrt_prober *prober, struct xrt_device **devices)
{
    (void)attached; (void)prober;
    struct hmd_prober *p=(struct hmd_prober *)base;
    const char *path=getenv("PICO_HMD_CONFIG"), *counter=getenv("PICO_HMD_COUNTER");
    if (p->attempted || no_hmds || !devices || devices[0] || !path || !*path || !counter || strcmp(counter,"1")) return 0;
    p->attempted=true;
    struct pico_hmd_config *config=NULL;
    bool accepted=pico_hmd_config_load(path,&config) && pico_monado_hmd_create(config,&devices[0]);
    pico_hmd_config_destroy(&config);
    if (!accepted) fprintf(stderr,"Pico HMD: calibration configuration or sensor session refused\n");
    return accepted ? 1 : 0;
}

static void destroy(struct xrt_auto_prober *base) { free(base); }

struct xrt_auto_prober *pico_hmd_create_auto_prober(void)
{
    struct hmd_prober *p=calloc(1,sizeof(*p));
    if (!p) return NULL;
    p->base.name="Pico HMD";
    p->base.lelo_dallas_autoprobe=autoprobe;p->base.destroy=destroy;
    return &p->base;
}
