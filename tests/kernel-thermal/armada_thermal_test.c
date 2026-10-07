#include <linux/module.h>
#include <linux/of.h>
#include <linux/thermal.h>

static int temperature = 25000;
static unsigned long state;
module_param(temperature, int, 0600);
module_param(state, ulong, 0400);
static struct thermal_zone_device *zone;
static struct thermal_cooling_device *fan;

static int get_temp(struct thermal_zone_device *tz, int *value)
{
	*value = READ_ONCE(temperature);
	return 0;
}

static int trip_type(struct thermal_zone_device *tz, int trip, enum thermal_trip_type *type)
{
	*type = trip == 0 ? THERMAL_TRIP_ACTIVE : THERMAL_TRIP_CRITICAL;
	return 0;
}

static int trip_temp(struct thermal_zone_device *tz, int trip, int *value)
{
	*value = trip == 0 ? 45000 : 65000;
	return 0;
}

static int trip_hyst(struct thermal_zone_device *tz, int trip, int *value)
{
	*value = trip == 0 ? 2000 : 1000;
	return 0;
}

static int max_state(struct thermal_cooling_device *cdev, unsigned long *value)
{
	*value = 4;
	return 0;
}

static int cur_state(struct thermal_cooling_device *cdev, unsigned long *value)
{
	*value = READ_ONCE(state);
	return 0;
}

static int set_state(struct thermal_cooling_device *cdev, unsigned long value)
{
	if (value > 4)
		return -EINVAL;
	WRITE_ONCE(state, value);
	return 0;
}

static int bind_fan(struct thermal_zone_device *tz, struct thermal_cooling_device *cdev)
{
	return cdev == fan ? thermal_zone_bind_cooling_device(tz, 0, cdev, 2, 2, THERMAL_WEIGHT_DEFAULT) : 0;
}

static int unbind_fan(struct thermal_zone_device *tz, struct thermal_cooling_device *cdev)
{
	return cdev == fan ? thermal_zone_unbind_cooling_device(tz, 0, cdev) : 0;
}

static struct thermal_zone_device_ops sensor_ops = {
	.get_temp = get_temp, .get_trip_type = trip_type, .get_trip_temp = trip_temp,
	.get_trip_hyst = trip_hyst, .bind = bind_fan, .unbind = unbind_fan,
};
static struct thermal_cooling_device_ops fan_ops = {
	.get_max_state = max_state, .get_cur_state = cur_state, .set_cur_state = set_state,
};
static struct thermal_zone_params params = { .governor_name = "step_wise", .no_hwmon = true };

static int __init thermal_test_init(void)
{
	int ret;
	if (!of_machine_is_compatible("linux,dummy-virt"))
		return -ENODEV;
	fan = thermal_cooling_device_register("armada-virtual-fan", NULL, &fan_ops);
	if (IS_ERR(fan))
		return PTR_ERR(fan);
	zone = thermal_zone_device_register("armada-test", 2, 0, NULL, &sensor_ops, &params, 100, 100);
	if (IS_ERR(zone)) {
		thermal_cooling_device_unregister(fan);
		return PTR_ERR(zone);
	}
	ret = thermal_zone_device_enable(zone);
	if (ret) {
		thermal_zone_device_unregister(zone);
		thermal_cooling_device_unregister(fan);
	}
	return ret;
}

static void __exit thermal_test_exit(void)
{
	thermal_zone_device_unregister(zone);
	thermal_cooling_device_unregister(fan);
}

module_init(thermal_test_init);
module_exit(thermal_test_exit);
MODULE_LICENSE("GPL");
