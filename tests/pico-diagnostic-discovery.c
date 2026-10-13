#define _POSIX_C_SOURCE 200809L
#include <libusb.h>
#include <assert.h>
#include <stdint.h>
#include <time.h>

#define MOCK(name) static __typeof__(name) mock_##name
MOCK(libusb_init);
MOCK(libusb_exit);
MOCK(libusb_get_device_list);
MOCK(libusb_free_device_list);
MOCK(libusb_get_device_descriptor);
MOCK(libusb_open);
MOCK(libusb_close);
MOCK(libusb_get_string_descriptor_ascii);
MOCK(libusb_get_active_config_descriptor);
MOCK(libusb_free_config_descriptor);
MOCK(libusb_claim_interface);
MOCK(libusb_release_interface);
MOCK(libusb_bulk_transfer);
MOCK(clock_gettime);
MOCK(nanosleep);
#undef MOCK
#define libusb_init mock_libusb_init
#define libusb_exit mock_libusb_exit
#define libusb_get_device_list mock_libusb_get_device_list
#define libusb_free_device_list mock_libusb_free_device_list
#define libusb_get_device_descriptor mock_libusb_get_device_descriptor
#define libusb_open mock_libusb_open
#define libusb_close mock_libusb_close
#define libusb_get_string_descriptor_ascii mock_libusb_get_string_descriptor_ascii
#define libusb_get_active_config_descriptor mock_libusb_get_active_config_descriptor
#define libusb_free_config_descriptor mock_libusb_free_config_descriptor
#define libusb_claim_interface mock_libusb_claim_interface
#define libusb_release_interface mock_libusb_release_interface
#define libusb_bulk_transfer mock_libusb_bulk_transfer
#define clock_gettime mock_clock_gettime
#define nanosleep mock_nanosleep
#define main diagnostic_main
#include "../tools/pico-diagnostic-log.c"
#undef main

static unsigned scans, ready_after, device_count, opened, closed, claims, released, lists, descriptors;
static time_t elapsed;
static int interface_class;
static const char *serial;
static libusb_device *devices[3];
static int mock_libusb_init(libusb_context **ctx) { *ctx = (libusb_context *)(uintptr_t)1; return 0; }
static void mock_libusb_exit(libusb_context *ctx) { (void)ctx; }
static ssize_t mock_libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    (void)ctx; ++scans; ++lists;
    for (unsigned i = 0; i < device_count; ++i) devices[i] = (libusb_device *)(uintptr_t)(i + 1);
    devices[device_count] = NULL; *list = devices; return device_count;
}
static void mock_libusb_free_device_list(libusb_device **list, int unref)
{
    assert(list == devices && unref == 1 && lists); --lists;
}
static int mock_libusb_get_device_descriptor(libusb_device *device, struct libusb_device_descriptor *value)
{
    (void)device; memset(value, 0, sizeof(*value));
    value->idVendor = 0x1d6b; value->idProduct = 0x0104;
    value->iManufacturer = 1; value->iProduct = 2; value->iSerialNumber = 3; return 0;
}
static int mock_libusb_open(libusb_device *device, libusb_device_handle **handle)
{
    ++opened; *handle = (libusb_device_handle *)device; return 0;
}
static void mock_libusb_close(libusb_device_handle *handle) { assert(handle); ++closed; }
static int mock_libusb_get_string_descriptor_ascii(libusb_device_handle *handle, uint8_t index, unsigned char *data, int length)
{
    (void)handle;
    const char *text = index == 1 ? "Reframed VR" : index == 2 ? "Reframed VR Pico diagnostic" : serial;
    assert((int)strlen(text) < length); memcpy(data, text, strlen(text)); return (int)strlen(text);
}
static int mock_libusb_get_active_config_descriptor(libusb_device *device, struct libusb_config_descriptor **result)
{
    (void)device;
    static struct libusb_endpoint_descriptor endpoints[2] = {
        {.bEndpointAddress = 0x81, .bmAttributes = LIBUSB_TRANSFER_TYPE_BULK},
        {.bEndpointAddress = 0x01, .bmAttributes = LIBUSB_TRANSFER_TYPE_BULK}
    };
    static struct libusb_interface_descriptor setting = {.bNumEndpoints = 2, .endpoint = endpoints};
    static struct libusb_interface interface = {.altsetting = &setting, .num_altsetting = 1};
    static struct libusb_config_descriptor config = {.bConfigurationValue = 1, .bNumInterfaces = 1, .interface = &interface};
    if (scans < ready_after) return LIBUSB_ERROR_NOT_FOUND;
    setting.bInterfaceClass = (uint8_t)interface_class; *result = &config; ++descriptors; return 0;
}
static void mock_libusb_free_config_descriptor(struct libusb_config_descriptor *config)
{
    assert(config && descriptors); --descriptors;
}
static int mock_libusb_claim_interface(libusb_device_handle *handle, int interface)
{
    assert(handle && interface == 0 && scans >= ready_after); ++claims; return 0;
}
static int mock_libusb_release_interface(libusb_device_handle *handle, int interface)
{
    assert(handle && interface == 0); ++released; return 0;
}
static int mock_libusb_bulk_transfer(libusb_device_handle *handle, unsigned char ep, unsigned char *data, int length, int *transferred, unsigned timeout)
{
    assert(handle && ep == 0x81 && timeout == 1000 && claims == 1);
    const char *report = "REFRAMED_PICO_DIAGNOSTIC_BEGIN kind=physical serial=test-device\nREFRAMED_PICO_DIAGNOSTIC_END\n";
    assert((int)strlen(report) < length); memcpy(data, report, strlen(report)); *transferred = (int)strlen(report); return 0;
}
static int mock_clock_gettime(clockid_t clock, struct timespec *now)
{
    assert(clock == CLOCK_MONOTONIC); *now = (struct timespec){.tv_sec = elapsed}; return 0;
}
static int mock_nanosleep(const struct timespec *delay, struct timespec *remaining)
{
    (void)remaining; assert(delay->tv_nsec == 250000000); ++elapsed; return 0;
}
static void scenario(unsigned count, unsigned ready, int class, const char *identity, int expected)
{
    scans = opened = closed = claims = released = lists = descriptors = 0;
    elapsed = 0; device_count = count; ready_after = ready; interface_class = class; serial = identity;
    char *args[] = {"diagnostic", "--serial", "test-device", NULL};
    assert(diagnostic_main(3, args) == expected);
    assert(opened == closed && claims == released && !lists && !descriptors);
    assert(claims == (expected == 0));
}
int main(void)
{
    scenario(1, 1, 0xff, "test-device", 0); assert(scans == 1);
    scenario(1, 5, 0xff, "test-device", 0); assert(scans == 5);
    scenario(1, 91, 0xff, "test-device", 1); assert(scans == 90);
    scenario(1, 1, 2, "test-device", 1); assert(scans == 90);
    scenario(1, 1, 0xff, "other-device", 1); assert(scans == 90);
    scenario(2, 1, 0xff, "test-device", 1); assert(scans == 1);
    scenario(2, 5, 0xff, "test-device", 1); assert(scans == 1);
    scenario(0, 1, 0xff, "test-device", 1); assert(scans == 90);
    puts("PICO_DIAGNOSTIC_DISCOVERY_PASS scenarios=8 USB_calls=0"); return 0;
}
