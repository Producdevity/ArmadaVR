#include "vxr7200-video.h"

#include <errno.h>

int vxr7200_read_video_status(const struct vxr7200_bus *bus,
                             struct vxr7200_video_status *status)
{
    static const uint32_t registers[] = {0x220830, 0x220838, 0x220814, 0x220804, 0x200008};
    uint32_t values[2][5];
    if (!bus || !bus->transfer || !status)
        return -EINVAL;
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < 5; ++i) {
            int result = vxr7200_read32(bus, registers[i], &values[pass][i]);
            if (result)
                return result;
        }
    for (unsigned i = 0; i < 5; ++i)
        if (values[0][i] != values[1][i])
            return -EAGAIN;
    struct vxr7200_video_status observed = {0};
    observed.horizontal_total = (uint16_t)values[0][0];
    observed.vertical_total = (uint16_t)(values[0][0] >> 16);
    observed.horizontal_active = (uint16_t)values[0][1];
    observed.vertical_active = (uint16_t)(values[0][1] >> 16);
    observed.frame_ticks = values[0][2];
    observed.clock_divider = (uint8_t)values[0][3];
    observed.clock_multiplier = (uint16_t)((values[0][4] >> 16) & 0x3ff);
    observed.stock_dimensions =
        (observed.vertical_active == 3840 && observed.horizontal_active == 2160) ||
        (observed.vertical_active == 3664 &&
         (observed.horizontal_active == 1920 || observed.horizontal_active == 3840));
    observed.rate_numerator = UINT64_C(1000000) * observed.clock_multiplier * observed.clock_divider;
    observed.rate_valid = observed.frame_ticks != 0 && observed.rate_numerator != 0;
    *status = observed;
    return 0;
}
