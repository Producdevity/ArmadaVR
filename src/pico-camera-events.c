#include "pico-camera-events.h"
#include <string.h>

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}
static void put(uint8_t *p, uint64_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

bool pico_camera_fence_payload(int32_t fence, uint64_t generation, uint64_t request,
                                void *output, size_t size)
{
    if (fence <= 0 || !generation || !request || request > INT64_MAX ||
        !output || size < PICO_CAMERA_FENCE_PAYLOAD_SIZE) return false;
    uint8_t bytes[PICO_CAMERA_FENCE_PAYLOAD_SIZE] = {0};
    put(bytes, (uint32_t)fence, 4);
    put(bytes + 8, generation, 8);
    put(bytes + 16, request, 8);
    memcpy(output, bytes, sizeof(bytes));
    return true;
}

enum pico_camera_event_result pico_camera_fence_event_decode(
    uint32_t type, uint32_t id, const void *data, size_t size, struct pico_camera_fence_event *event)
{
    if (type != 0x08000000 || id != 0) return PICO_CAMERA_EVENT_UNRELATED;
    if (!data || !event || size != PICO_CAMERA_EVENT_DATA_SIZE) return PICO_CAMERA_EVENT_MALFORMED;
    const uint8_t *p = data;
    uint32_t fence = get32(p), status = get32(p + 4);
    uint64_t generation = get64(p + 8), request = get64(p + 16);
    if (!fence || fence > INT32_MAX || (status != 2 && status != 3) ||
        !generation || !request || request > INT64_MAX) return PICO_CAMERA_EVENT_MALFORMED;
    *event = (struct pico_camera_fence_event){(int32_t)fence, status, generation, request};
    return PICO_CAMERA_EVENT_VALID;
}

enum pico_camera_event_result pico_camera_sof_event_decode(
    uint32_t type, uint32_t id, const void *data, size_t size, struct pico_camera_sof_event *event)
{
    if (type != 0x08000000 || id != 0) return PICO_CAMERA_EVENT_UNRELATED;
    if (!data || !event || size != PICO_CAMERA_EVENT_DATA_SIZE) return PICO_CAMERA_EVENT_MALFORMED;
    const uint8_t *p = data;
    uint32_t session = get32(p), link = get32(p + 32), status = get32(p + 36);
    uint64_t request = get64(p + 8), time = get64(p + 24);
    if (!session || session > INT32_MAX || !link || link > INT32_MAX || status > 1 ||
        request > INT64_MAX || time > INT64_MAX || (!time && !status)) return PICO_CAMERA_EVENT_MALFORMED;
    *event = (struct pico_camera_sof_event){(int32_t)session, (int32_t)link, request,
        get64(p + 16), (int64_t)time, status};
    return PICO_CAMERA_EVENT_VALID;
}

enum pico_camera_event_result pico_camera_error_event_decode(
    uint32_t type, uint32_t id, const void *data, size_t size, struct pico_camera_error_event *event)
{
    if (type != 0x08000000 || id != 1) return PICO_CAMERA_EVENT_UNRELATED;
    if (!data || !event || size != PICO_CAMERA_EVENT_DATA_SIZE) return PICO_CAMERA_EVENT_MALFORMED;
    const uint8_t *p = data;
    uint32_t session = get32(p), device = get32(p + 16), link = get32(p + 20), error = get32(p + 8);
    if (!session || session > INT32_MAX || !link || link > INT32_MAX || error > 5 ||
        device > INT32_MAX) return PICO_CAMERA_EVENT_MALFORMED;
    *event = (struct pico_camera_error_event){(int32_t)session, (int32_t)device, (int32_t)link,
        error, get32(p + 12), get64(p + 24)};
    return PICO_CAMERA_EVENT_VALID;
}
