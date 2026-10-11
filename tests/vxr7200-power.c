#include "vxr7200-power.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct event { int rail, value; };
struct fixture {
    struct event events[32];
    unsigned count, writes, failures;
    int error;
    bool rails[3];
};

static int set_rail(void *context, enum vxr7200_rail rail, bool enabled)
{
    struct fixture *fixture = context;
    assert(fixture->count < 32 && rail <= VXR7200_RAIL_1V0);
    fixture->events[fixture->count++] = (struct event){rail, enabled};
    fixture->rails[rail] = enabled;
    return fixture->failures & (1u << fixture->writes++) ? fixture->error : 0;
}

static void delay_ms(void *context, unsigned milliseconds)
{
    struct fixture *fixture = context;
    assert(fixture->count < 32 && milliseconds == 5);
    assert(fixture->rails[VXR7200_RAIL_1V8]);
    assert(fixture->rails[VXR7200_RAIL_3V3]);
    assert(!fixture->rails[VXR7200_RAIL_1V0]);
    fixture->events[fixture->count++] = (struct event){-1, (int)milliseconds};
}

static void check_events(const struct fixture *fixture, const struct event *events, unsigned count)
{
    assert(fixture->count == count);
    for (unsigned i = 0; i < count; ++i) {
        assert(fixture->events[i].rail == events[i].rail);
        assert(fixture->events[i].value == events[i].value);
    }
}

int main(void)
{
    const struct event off[] = {{VXR7200_RAIL_3V3, 0}, {VXR7200_RAIL_1V0, 0}, {VXR7200_RAIL_1V8, 0}};
    const struct event on[] = {{VXR7200_RAIL_1V8, 1}, {VXR7200_RAIL_3V3, 1}, {-1, 5}, {VXR7200_RAIL_1V0, 1}};
    struct fixture fixture = {0};
    struct vxr7200_power power = {&fixture, set_rail, delay_ms, VXR7200_POWER_UNKNOWN};
    assert(vxr7200_power_set(NULL, true) == -EINVAL);
    assert(vxr7200_power_set(&power, true) == -EAGAIN && fixture.count == 0);
    assert(vxr7200_power_set(&power, false) == 0 && power.state == VXR7200_POWER_OFF);
    check_events(&fixture, off, 3);
    assert(vxr7200_power_set(&power, false) == 0 && fixture.count == 3);
    memset(&fixture, 0, sizeof(fixture));
    assert(vxr7200_power_set(&power, true) == 0 && power.state == VXR7200_POWER_ON);
    check_events(&fixture, on, 4);
    assert(vxr7200_power_set(&power, true) == 0 && fixture.count == 4);
    memset(&fixture, 0, sizeof(fixture));
    assert(vxr7200_power_set(&power, false) == 0 && power.state == VXR7200_POWER_OFF);
    check_events(&fixture, off, 3);

    unsigned cases = 0;
    const int errors[] = {-EIO, -ETIMEDOUT, -ENODEV, 1};
    for (unsigned e = 0; e < sizeof(errors) / sizeof(errors[0]); ++e) {
        for (unsigned failed_step = 0; failed_step < 3; ++failed_step) {
            for (unsigned cleanup_errors = 0; cleanup_errors < 8; ++cleanup_errors) {
                memset(&fixture, 0, sizeof(fixture));
                fixture.error = errors[e];
                fixture.failures = (1u << failed_step) | (cleanup_errors << (failed_step + 1));
                power.state = VXR7200_POWER_OFF;
                assert(vxr7200_power_set(&power, true) == (errors[e] > 0 ? -EIO : errors[e]));
                assert(fixture.writes == failed_step + 4);
                unsigned before_off = failed_step + 1 + (failed_step == 2);
                assert(fixture.count == before_off + 3);
                for (unsigned i = 0; i < before_off; ++i) {
                    assert(fixture.events[i].rail == on[i].rail);
                    assert(fixture.events[i].value == on[i].value);
                }
                for (unsigned i = 0; i < 3; ++i) {
                    assert(fixture.events[before_off + i].rail == off[i].rail);
                    assert(fixture.events[before_off + i].value == 0);
                }
                assert(power.state == (cleanup_errors ? VXR7200_POWER_UNKNOWN : VXR7200_POWER_OFF));
                if (cleanup_errors) {
                    unsigned count = fixture.count;
                    assert(vxr7200_power_set(&power, true) == -EAGAIN && fixture.count == count);
                }
                memset(&fixture, 0, sizeof(fixture));
                assert(vxr7200_power_set(&power, false) == 0 && power.state == VXR7200_POWER_OFF);
                assert(vxr7200_power_set(&power, true) == 0 && power.state == VXR7200_POWER_ON);
                ++cases;
            }
        }
        for (unsigned failures = 1; failures < 8; ++failures) {
            memset(&fixture, 0, sizeof(fixture));
            fixture.failures = failures;
            fixture.error = errors[e];
            power.state = VXR7200_POWER_ON;
            assert(vxr7200_power_set(&power, false) == (errors[e] > 0 ? -EIO : errors[e]));
            check_events(&fixture, off, 3);
            assert(power.state == VXR7200_POWER_UNKNOWN);
            ++cases;
        }
    }
    power.state = (enum vxr7200_power_state)99;
    unsigned count = fixture.count;
    assert(vxr7200_power_set(&power, false) == -EINVAL && fixture.count == count);
    power.state = VXR7200_POWER_OFF;
    power.set_rail = NULL;
    assert(vxr7200_power_set(&power, true) == -EINVAL && fixture.count == count);
    power.set_rail = set_rail;
    power.delay_ms = NULL;
    assert(vxr7200_power_set(&power, true) == -EINVAL && fixture.count == count);
    printf("%u VXR7200 power failure scenarios passed\n", cases);
    return 0;
}
