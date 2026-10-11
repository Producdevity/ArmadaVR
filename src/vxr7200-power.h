#ifndef ARMADA_VXR7200_POWER_H
#define ARMADA_VXR7200_POWER_H

#include <stdbool.h>

enum vxr7200_rail {
    VXR7200_RAIL_1V8,
    VXR7200_RAIL_3V3,
    VXR7200_RAIL_1V0,
};

enum vxr7200_power_state {
    VXR7200_POWER_UNKNOWN,
    VXR7200_POWER_OFF,
    VXR7200_POWER_ON,
};

struct vxr7200_power {
    void *context;
    int (*set_rail)(void *context, enum vxr7200_rail rail, bool enabled);
    void (*delay_ms)(void *context, unsigned milliseconds);
    enum vxr7200_power_state state;
};

/* The caller owns the GPIOs, holds bridge reset asserted, and serializes every
 * operation with panel/mux and suspend control. Initialize state to UNKNOWN.
 * Callbacks are synchronous: set_rail returns zero or a negative errno and
 * delay_ms waits at least the requested interval. No panel or mux is changed. */
int vxr7200_power_set(struct vxr7200_power *power, bool enabled);

#endif
