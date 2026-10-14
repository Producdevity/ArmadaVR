#define _POSIX_C_SOURCE 200809L
#define XR_USE_TIMESPEC
#include <time.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR(call) do { XrResult r = (call); if (XR_FAILED(r)) { \
    fprintf(stderr, "%s: %d\n", #call, r); exit(1); } } while (0)

static double now(void)
{
    struct timespec ts;
    assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void delay(void)
{
    struct timespec ts = {.tv_nsec = 20000000};
    nanosleep(&ts, NULL);
}

int main(void)
{
    char board[64] = {0}, cmdline[4096] = {0};
    FILE *f = fopen("/proc/device-tree/compatible", "r");
    assert(f && fgets(board, sizeof(board), f)); fclose(f);
    f = fopen("/proc/cmdline", "r");
    assert(f && fgets(cmdline, sizeof(cmdline), f)); fclose(f);
    assert(!strcmp(board, "linux,dummy-virt") && strstr(cmdline, "reframed.ssc_test=1"));
    const char *extensions[] = {"XR_MND_headless", "XR_KHR_convert_timespec_time"};
    XrInstanceCreateInfo create = {.type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {.applicationName = "Pico HMD integration", .apiVersion = XR_API_VERSION_1_0},
        .enabledExtensionCount = 2, .enabledExtensionNames = extensions};
    XrInstance instance;
    XR(xrCreateInstance(&create, &instance));
    PFN_xrConvertTimespecTimeToTimeKHR convert;
    XR(xrGetInstanceProcAddr(instance, "xrConvertTimespecTimeToTimeKHR", (PFN_xrVoidFunction *)&convert));
    XrSystemGetInfo get = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId system;
    XR(xrGetSystem(instance, &get, &system));
    XrSystemProperties properties = {.type = XR_TYPE_SYSTEM_PROPERTIES};
    XR(xrGetSystemProperties(instance, system, &properties));
    assert(strstr(properties.systemName, "Pico Neo3 HMD"));
    assert(properties.trackingProperties.orientationTracking && !properties.trackingProperties.positionTracking);
    XrSessionCreateInfo session_create = {.type = XR_TYPE_SESSION_CREATE_INFO, .systemId = system};
    XrSession session;
    XR(xrCreateSession(instance, &session_create, &session));
    bool running = false, focused = false;
    double deadline = now() + 15;
    while (!focused && now() < deadline) {
        XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = xrPollEvent(instance, &event);
        XR(result);
        if (result == XR_SUCCESS && event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrSessionState state = ((XrEventDataSessionStateChanged *)&event)->state;
            if (state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin = {.type = XR_TYPE_SESSION_BEGIN_INFO,
                    .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                XR(xrBeginSession(session, &begin)); running = true;
            }
            if (state == XR_SESSION_STATE_FOCUSED) focused = true;
        }
        delay();
    }
    assert(running && focused);
    XrReferenceSpaceCreateInfo space_create = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, .poseInReferenceSpace.orientation.w = 1};
    XrSpace local, head;
    XR(xrCreateReferenceSpace(session, &space_create, &local));
    space_create.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR(xrCreateReferenceSpace(session, &space_create, &head));
    XrQuaternionf first = {0};
    unsigned accepted = 0, predicted = 0;
    bool moved = false;
    deadline = now() + 20;
    while ((!moved || accepted < 10 || predicted < 10) && now() < deadline) {
        struct timespec ts;
        assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
        XrTime time;
        XR(convert(instance, &ts, &time));
        XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
        XR(xrLocateSpace(head, local, time, &location));
        assert(!(location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT));
        if ((location.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) ==
            (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) {
            XrQuaternionf q = location.pose.orientation;
            float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
            assert(isfinite(norm) && fabsf(norm - 1) < 0.001f);
            if (!accepted) first = q;
            float dot = q.x*first.x + q.y*first.y + q.z*first.z + q.w*first.w;
            moved |= fabsf(dot) < 0.999f;
            XrViewLocateInfo locate = {.type = XR_TYPE_VIEW_LOCATE_INFO,
                .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, .displayTime = time, .space = head};
            XrViewState state = {.type = XR_TYPE_VIEW_STATE};
            XrView views[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
            uint32_t count;
            XR(xrLocateViews(session, &locate, &state, 2, &count, views));
            assert(count == 2);
            for (unsigned i = 0; i < 2; i++) {
                assert(fabsf(views[i].fov.angleLeft + 0.7f) < 0.0001f);
                assert(fabsf(views[i].fov.angleRight - 0.7f) < 0.0001f);
                assert(fabsf(views[i].fov.angleUp - 0.65f) < 0.0001f);
                assert(fabsf(views[i].fov.angleDown + 0.65f) < 0.0001f);
                assert(fabsf(views[i].pose.position.x - (i ? 0.032f : -0.032f)) < 0.0001f);
            }
            XrSpaceVelocity velocity = {.type = XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation future = {.type = XR_TYPE_SPACE_LOCATION, .next = &velocity};
            XR(xrLocateSpace(head, local, time + 20000000, &future));
            if ((future.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) ==
                (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) {
                XrQuaternionf later = future.pose.orientation;
                float relative_y = -later.w*q.y + later.y*q.w - later.z*q.x + later.x*q.z;
                assert(fabsf(relative_y - sinf(0.35f * 0.02f / 2)) < 0.0001f);
                assert(velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT);
                assert(!(velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT));
                assert(fabsf(velocity.angularVelocity.x) < 0.0001f &&
                       fabsf(velocity.angularVelocity.y - 0.35f) < 0.0001f &&
                       fabsf(velocity.angularVelocity.z) < 0.0001f);
                predicted++;
            }
            XrSpaceLocation distant = {.type = XR_TYPE_SPACE_LOCATION};
            XR(xrLocateSpace(head, local, time + 5000000000LL, &distant));
            assert(!(distant.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)));
            accepted++;
        }
        delay();
    }
    printf("PICO_HMD_ORIENTATION samples=%u moved=%d\n", accepted, moved); fflush(stdout);
    assert(accepted >= 10 && moved && predicted >= 10);
    printf("PICO_HMD_PREDICTION samples=%u bounded=1 angular_velocity=1\n", predicted); fflush(stdout);
    f = fopen("/hmd-phase", "w"); assert(f && fputs("stale", f) >= 0); assert(!fclose(f));
    unsigned invalid = 0;
    deadline = now() + 5;
    while (invalid < 5 && now() < deadline) {
        struct timespec ts;
        assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
        XrTime time;
        XR(convert(instance, &ts, &time));
        XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
        XR(xrLocateSpace(head, local, time, &location));
        invalid = (location.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT)) ? 0 : invalid + 1;
        delay();
    }
    assert(invalid == 5);
    XR(xrDestroySpace(head)); XR(xrDestroySpace(local));
    XR(xrDestroySession(session)); XR(xrDestroyInstance(instance));
    puts("PICO_OPENXR_HMD_PASS calibrated_views=2 position_tracking=0 stale_invalid=1");
    return 0;
}
