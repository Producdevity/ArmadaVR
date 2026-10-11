#include "vxr7200-power.h"

#include <errno.h>

static int set_rail(struct vxr7200_power *power, enum vxr7200_rail rail, bool enabled)
{
    int result = power->set_rail(power->context, rail, enabled);
    return result > 0 ? -EIO : result;
}

static int power_off(struct vxr7200_power *power)
{
    static const enum vxr7200_rail order[] = {
        VXR7200_RAIL_3V3, VXR7200_RAIL_1V0, VXR7200_RAIL_1V8,
    };
    int first_error = 0;
    power->state = VXR7200_POWER_UNKNOWN;
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        int result = set_rail(power, order[i], false);
        if (result && !first_error)
            first_error = result;
    }
    if (!first_error)
        power->state = VXR7200_POWER_OFF;
    return first_error;
}

int vxr7200_power_set(struct vxr7200_power *power, bool enabled)
{
    if (!power || !power->set_rail || !power->delay_ms ||
        power->state < VXR7200_POWER_UNKNOWN || power->state > VXR7200_POWER_ON)
        return -EINVAL;
    if (!enabled)
        return power->state == VXR7200_POWER_OFF ? 0 : power_off(power);
    if (power->state == VXR7200_POWER_ON)
        return 0;
    if (power->state != VXR7200_POWER_OFF)
        return -EAGAIN;

    power->state = VXR7200_POWER_UNKNOWN;
    int result = set_rail(power, VXR7200_RAIL_1V8, true);
    if (!result)
        result = set_rail(power, VXR7200_RAIL_3V3, true);
    if (!result) {
        power->delay_ms(power->context, 5);
        result = set_rail(power, VXR7200_RAIL_1V0, true);
    }
    if (result) {
        /* A failed GPIO operation can still have changed its output. */
        power_off(power);
        return result;
    }
    power->state = VXR7200_POWER_ON;
    return 0;
}
