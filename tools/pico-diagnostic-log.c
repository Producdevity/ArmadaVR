#define _POSIX_C_SOURCE 200809L
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int string_matches(libusb_device_handle *handle, unsigned char index, const char *expected)
{
    unsigned char text[256];
    int count = libusb_get_string_descriptor_ascii(handle, index, text, sizeof(text));
    return count >= 0 && (size_t)count == strlen(expected) && !memcmp(text, expected, (size_t)count);
}

static int interface(libusb_device *device, unsigned char *endpoint)
{
    struct libusb_config_descriptor *config = NULL;
    if (libusb_get_active_config_descriptor(device, &config)) return 0;
    int valid = config->bConfigurationValue == 1 && config->bNumInterfaces == 1 &&
        config->interface[0].num_altsetting == 1;
    if (valid) {
        const struct libusb_interface_descriptor *value = config->interface[0].altsetting;
        valid = value->bInterfaceNumber == 0 && value->bAlternateSetting == 0 &&
            value->bInterfaceClass == 0xff && value->bInterfaceSubClass == 0 &&
            value->bInterfaceProtocol == 0 && value->bNumEndpoints == 2;
        int inputs = 0, outputs = 0;
        for (int i = 0; valid && i < 2; ++i) {
            const struct libusb_endpoint_descriptor *ep = value->endpoint + i;
            valid = (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK;
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) { ++inputs; *endpoint = ep->bEndpointAddress; }
            else ++outputs;
        }
        valid = valid && inputs == 1 && outputs == 1;
    }
    libusb_free_config_descriptor(config);
    return valid;
}

static double seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) exit(1);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static int complete(const unsigned char *data, size_t size, const char *begin)
{
    const char *end = "ARMADA_PICO_DIAGNOSTIC_END\n";
    return size >= strlen(begin) + strlen(end) && !memcmp(data, begin, strlen(begin)) &&
        !memcmp(data + size - strlen(end), end, strlen(end));
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--serial") || !strlen(argv[2]) || strlen(argv[2]) > 64 ||
        strspn(argv[2], "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != strlen(argv[2]) ||
        !strcmp(argv[2], "qemu")) {
        fputs("Usage: pico-diagnostic-log --serial HEADSET_SERIAL > NEW_LOG_FILE\n", stderr);
        return 2;
    }
    libusb_context *context = NULL;
    libusb_device **devices = NULL;
    libusb_device_handle *selected = NULL;
    unsigned char endpoint = 0;
    int claimed = 0, result = 1, matches = 0;
    if (libusb_init(&context)) return 1;
    double discovery_deadline = seconds() + 90;
    do {
        ssize_t count = libusb_get_device_list(context, &devices);
        if (count < 0) goto done;
        for (ssize_t i = 0; i < count; ++i) {
            struct libusb_device_descriptor descriptor;
            if (libusb_get_device_descriptor(devices[i], &descriptor) || descriptor.idVendor != 0x1d6b || descriptor.idProduct != 0x0104) continue;
            libusb_device_handle *handle = NULL;
            if (libusb_open(devices[i], &handle)) continue;
            if (string_matches(handle, descriptor.iManufacturer, "ArmadaVR") &&
                string_matches(handle, descriptor.iProduct, "ArmadaVR Pico diagnostic") &&
                string_matches(handle, descriptor.iSerialNumber, argv[2])) {
                ++matches;
                if (!selected) {
                    selected = handle;
                    if (!interface(devices[i], &endpoint)) goto done;
                    continue;
                }
            }
            libusb_close(handle);
        }
        if (matches) break;
        libusb_free_device_list(devices, 1);
        devices = NULL;
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 250000000};
        nanosleep(&delay, NULL);
    } while (seconds() < discovery_deadline);
    if (matches != 1 || !selected || libusb_claim_interface(selected, 0)) {
        fputs("No unique available ArmadaVR diagnostic interface; no driver was detached.\n", stderr);
        goto done;
    }
    claimed = 1;
    unsigned char data[128 * 1024 + 1];
    size_t used = 0;
    double deadline = seconds() + 75;
    char begin[160];
    snprintf(begin, sizeof(begin), "ARMADA_PICO_DIAGNOSTIC_BEGIN kind=physical serial=%s\n", argv[2]);
    while (used < sizeof(data) - 1 && seconds() < deadline) {
        int transferred = 0;
        int bytes = (int)(sizeof(data) - 1 - used);
        if (bytes > 4096) bytes = 4096;
        int status = libusb_bulk_transfer(selected, endpoint, data + used, bytes, &transferred, 1000);
        if (transferred < 0 || transferred > bytes) break;
        used += (size_t)transferred;
        data[used] = 0;
        if (used >= strlen(begin) && memcmp(data, begin, strlen(begin))) break;
        if (complete(data, used, begin)) { result = 0; break; }
        if (status && status != LIBUSB_ERROR_TIMEOUT) break;
    }
    if (fwrite(data, 1, used, stdout) != used || fflush(stdout)) result = 1;
    if (result) fputs("Incomplete or mismatched diagnostic; partial log retained on stdout.\n", stderr);
done:
    if (claimed) libusb_release_interface(selected, 0);
    if (selected) libusb_close(selected);
    if (devices) libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return result;
}
