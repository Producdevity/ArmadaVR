#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define __user
#define CAM_ERR(...) ((void)0)
#define CAM_DBG(...) ((void)0)
#define CAM_INFO(...) ((void)0)
#define CAM_HANDLE_USER_POINTER 1
#define CAM_STOP_DEV 6
#define CAM_CSIPHY_START 2
#define CAM_CSIPHY_ACQUIRE 1
#define CAM_SECURE_MODE_NON_SECURE 0

struct cam_hw_soc_info { int index; };
struct csiphy_device {
    int mutex, ref_count, acquire_count, csiphy_state, cpas_handle;
    uint32_t start_dev_count;
    uint8_t session_max_device_support;
    struct cam_hw_soc_info soc_info;
    struct {
        struct { int32_t device_hdl; } hdl_data;
        int secure_mode;
        uint32_t csiphy_cpas_cp_reg_mask;
        uint64_t data_rate, settle_time;
    } csiphy_info[3];
};
struct cam_control { uint32_t op_code, handle_type; uint64_t handle; };
struct cam_start_stop_dev_cmd { int32_t session_handle, dev_handle; };

static unsigned cases, defects, lock_count, unlock_count, reset_calls, disable_calls;
static unsigned cpas_calls, secure_calls, lane_calls, missing_bytes;
static int disable_result, cpas_result, secure_result, lane_result;
static void check(bool value) { defects += !value; }
static void mutex_lock(int *lock) { check(!*lock); *lock = 1; ++lock_count; }
static void mutex_unlock(int *lock) { check(*lock == 1); *lock = 0; ++unlock_count; }
static int copy_from_user(void *to, const void *from, size_t size)
{
    check(missing_bytes <= size);
    memset(to, 0, size);
    memcpy(to, from, size - missing_bytes);
    return (int)missing_bytes;
}
static void cam_csiphy_reset(struct csiphy_device *device)
{
    check(device->ref_count == 0);
    ++reset_calls;
}
static int cam_soc_util_disable_platform_resource(struct cam_hw_soc_info *soc, bool clocks, bool irq)
{
    check(soc && clocks && irq && reset_calls == 1);
    ++disable_calls;
    return disable_result;
}
static int cam_cpas_stop(int handle)
{
    check(handle == 123 && reset_calls == 1 && disable_calls == 1);
    ++cpas_calls;
    return cpas_result;
}
static int cam_csiphy_notify_secure_mode(struct csiphy_device *device, bool protect, int offset)
{
    check(!protect && offset >= 0 && offset < device->acquire_count);
    ++secure_calls;
    return secure_result;
}
static int cam_csiphy_update_lane(struct csiphy_device *device, int offset, bool enable)
{
    check(!enable && offset >= 0 && offset < device->acquire_count);
    ++lane_calls;
    return lane_result;
}

#include "techpack/camera/drivers/cam_sensor_module/cam_csiphy/cam_csiphy_soc.c"
#include "techpack/camera/drivers/cam_sensor_module/cam_csiphy/cam_csiphy_core.c"

static struct csiphy_device device(void)
{
    lock_count = unlock_count = reset_calls = disable_calls = 0;
    cpas_calls = secure_calls = lane_calls = missing_bytes = 0;
    disable_result = cpas_result = secure_result = lane_result = 0;
    struct csiphy_device d = {.ref_count = 1, .acquire_count = 2,
        .csiphy_state = CAM_CSIPHY_START, .cpas_handle = 123,
        .start_dev_count = 1, .session_max_device_support = 2};
    d.csiphy_info[0].hdl_data.device_hdl = 31;
    d.csiphy_info[1].hdl_data.device_hdl = 32;
    d.csiphy_info[0].csiphy_cpas_cp_reg_mask = 7;
    return d;
}
static int stop(struct csiphy_device *d, int handle)
{
    struct cam_start_stop_dev_cmd stop = {.session_handle = 8, .dev_handle = handle};
    struct cam_control cmd = {CAM_STOP_DEV, CAM_HANDLE_USER_POINTER, (uintptr_t)&stop};
    int result = cam_csiphy_core_cfg(d, &cmd);
    check(!d->mutex && lock_count == 1 && unlock_count == 1);
    ++cases;
    return result;
}
int main(void)
{
    const int errors[] = {0, -EIO, -ETIMEDOUT, -EINTR};
    for (unsigned secure = 0; secure < 2; ++secure)
        for (unsigned a = 0; a < 4; ++a)
            for (unsigned b = 0; b < 4; ++b)
                for (unsigned c = 0; c < 4; ++c) {
                    struct csiphy_device d = device();
                    d.csiphy_info[0].secure_mode = secure;
                    secure_result = errors[a]; disable_result = errors[b]; cpas_result = errors[c];
                    int expected = secure && secure_result ? secure_result :
                                   disable_result ? disable_result : cpas_result;
                    check(stop(&d, 31) == expected);
                    check(reset_calls == 1 && disable_calls == 1 && cpas_calls == 1);
                    check(secure_calls == secure && !lane_calls && !d.ref_count && !d.start_dev_count);
                    check(d.csiphy_state == CAM_CSIPHY_ACQUIRE && !d.csiphy_info[0].secure_mode &&
                          !d.csiphy_info[0].csiphy_cpas_cp_reg_mask);
                }
    for (unsigned secure = 0; secure < 2; ++secure)
        for (unsigned a = 0; a < 4; ++a)
            for (unsigned b = 0; b < 4; ++b) {
                struct csiphy_device d = device();
                d.start_dev_count = 2; d.csiphy_info[0].secure_mode = secure;
                secure_result = errors[a]; lane_result = errors[b];
                int expected = secure && secure_result ? secure_result : lane_result;
                check(stop(&d, 31) == expected);
                check(!reset_calls && !disable_calls && !cpas_calls && lane_calls == 1);
                check(secure_calls == secure && d.ref_count == 1 && d.start_dev_count == 1);
                check(d.csiphy_state == CAM_CSIPHY_START);
            }
    for (unsigned fault = 0; fault < 12; ++fault) {
        struct csiphy_device d = device();
        int handle = 31, expected = -EINVAL;
        if (fault < 8) { missing_bytes = fault + 1; expected = -EFAULT; }
        if (fault == 8) d.csiphy_state = CAM_CSIPHY_ACQUIRE;
        if (fault == 9) d.start_dev_count = 0;
        if (fault == 10) handle = 99;
        if (fault == 11) { d.acquire_count = 1; handle = 32; }
        struct csiphy_device before = d;
        check(stop(&d, handle) == expected);
        check(!memcmp(&d, &before, sizeof(d)));
        check(!reset_calls && !disable_calls && !cpas_calls && !secure_calls && !lane_calls);
    }
    for (unsigned ref = 0; ref < 3; ++ref)
        for (unsigned error = 0; error < 4; ++error) {
            struct csiphy_device d = device();
            d.ref_count = (int)ref; disable_result = errors[error];
            check(cam_csiphy_disable_hw(&d) == (ref == 1 ? disable_result : 0));
            check(reset_calls == (ref == 1) && disable_calls == (ref == 1));
            check(d.ref_count == (ref ? (int)ref - 1 : 0));
            ++cases;
        }
    printf("PICO_CSIPHY_STOP cases=%u defects=%u\n", cases, defects);
    return defects != 0;
}
