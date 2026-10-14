#include "pico-camera-control.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

static unsigned calls, failure;
static uint32_t event_id;
static bool enabled;
int ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request); uint8_t *data = va_arg(args, void *); va_end(args);
    assert(fd == 31); ++calls;
    if (request == 0x80885659UL) {
        for (unsigned i = 0; i < 136; ++i) assert(!data[i]);
        memset(data, 0xa5, 136);
        uint32_t type = 0x08000000, pending = 17, sequence = UINT32_MAX;
        memcpy(data, &type, 4); memcpy(data + 72, &pending, 4);
        memcpy(data + 76, &sequence, 4); memcpy(data + 96, &event_id, 4);
        for (unsigned i = 0; i < 64; ++i) data[8 + i] = (uint8_t)i;
    } else {
        assert(request == (enabled ? 0x4020565aUL : 0x4020565bUL));
        uint32_t words[8]; memcpy(words, data, sizeof(words));
        assert(words[0] == 0x08000000 && words[1] == event_id);
        for (unsigned i = 2; i < 8; ++i) assert(!words[i]);
    }
    if (failure) { errno = failure == 1 ? EAGAIN : EINTR; return -1; }
    return 0;
}
int main(void)
{
    for (event_id = 0; event_id < 2; ++event_id) {
        enabled = true; assert(!pico_camera_event_subscribe(31, event_id, true));
        struct pico_camera_event event = {0};
        assert(!pico_camera_event_dequeue(31, &event));
        assert(event.type == 0x08000000 && event.id == event_id && event.sequence == UINT32_MAX && event.pending == 17);
        for (unsigned i = 0; i < 64; ++i) assert(event.data[i] == i);
        enabled = false; assert(!pico_camera_event_subscribe(31, event_id, false));
    }
    event_id = 0; struct pico_camera_event event, original;
    memset(&event, 0x5a, sizeof(event)); original = event;
    for (failure = 1; failure <= 2; ++failure) {
        unsigned before = calls;
        assert(pico_camera_event_dequeue(31, &event) == (failure == 1 ? -EAGAIN : -EINTR));
        assert(calls == before + 1 && !memcmp(&event, &original, sizeof(event)));
    }
    unsigned before = calls;
    assert(pico_camera_event_subscribe(31, 2, true) == -EINVAL);
    assert(pico_camera_event_subscribe(-1, 0, true) == -EINVAL);
    assert(pico_camera_event_dequeue(-1, &event) == -EINVAL);
    assert(pico_camera_event_dequeue(31, NULL) == -EINVAL);
    assert(calls == before);
    printf("PICO_CAMERA_EVENT_IO_PASS calls=%u hardware=0\n", calls); return 0;
}
