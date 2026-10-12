#define _POSIX_C_SOURCE 200809L
#include <openxr/openxr.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XR(call) do { XrResult r = (call); if (XR_FAILED(r)) { \
    fprintf(stderr, "%s: %d\n", #call, r); exit(1); } } while (0)

static XrInstance instance;

static XrPath path(const char *name)
{
    XrPath value;
    XR(xrStringToPath(instance, name, &value));
    return value;
}

static double now(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void phase(const char *name)
{
    FILE *file = fopen("/controller-phase", "w");
    assert(file);
    assert(fputs(name, file) >= 0);
    assert(fclose(file) == 0);
}

int main(void)
{
    char board[64] = {0}, cmdline[4096] = {0};
    FILE *file = fopen("/proc/device-tree/compatible", "r");
    assert(file && fgets(board, sizeof(board), file));
    fclose(file);
    file = fopen("/proc/cmdline", "r");
    assert(file && fgets(cmdline, sizeof(cmdline), file));
    fclose(file);
    assert(!strcmp(board, "linux,dummy-virt") && strstr(cmdline, "armada.ssc_test=1"));

    const char *extensions[] = {"XR_MND_headless", "XR_BD_controller_interaction"};
    XrInstanceCreateInfo create = {.type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {.applicationName = "Pico controller integration", .apiVersion = XR_API_VERSION_1_0},
        .enabledExtensionCount = 2, .enabledExtensionNames = extensions};
    XR(xrCreateInstance(&create, &instance));
    XrSystemGetInfo get = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId system;
    XR(xrGetSystem(instance, &get, &system));
    XrSessionCreateInfo session_create = {.type = XR_TYPE_SESSION_CREATE_INFO, .systemId = system};
    XrSession session;
    XR(xrCreateSession(instance, &session_create, &session));
    XrActionSetCreateInfo set_create = {.type = XR_TYPE_ACTION_SET_CREATE_INFO,
        .actionSetName = "controllers", .localizedActionSetName = "Controllers"};
    XrActionSet set;
    XR(xrCreateActionSet(instance, &set_create, &set));
    const XrPath hands[] = {path("/user/hand/left"), path("/user/hand/right")};
    const char *names[] = {"button", "touch", "trigger", "pose"};
    const XrActionType types[] = {XR_ACTION_TYPE_BOOLEAN_INPUT, XR_ACTION_TYPE_BOOLEAN_INPUT,
        XR_ACTION_TYPE_FLOAT_INPUT, XR_ACTION_TYPE_POSE_INPUT};
    XrAction actions[4];
    for (unsigned i = 0; i < 4; i++) {
        XrActionCreateInfo action_create = {.type = XR_TYPE_ACTION_CREATE_INFO,
            .actionType = types[i], .countSubactionPaths = 2, .subactionPaths = hands};
        strcpy(action_create.actionName, names[i]);
        strcpy(action_create.localizedActionName, names[i]);
        XR(xrCreateAction(set, &action_create, &actions[i]));
    }
    const char *bindings[4][2] = {
        {"/user/hand/left/input/x/click", "/user/hand/right/input/a/click"},
        {"/user/hand/left/input/x/touch", "/user/hand/right/input/a/touch"},
        {"/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value"},
        {"/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"},
    };
    XrActionSuggestedBinding suggested[8];
    for (unsigned i = 0; i < 4; i++)
        for (unsigned hand = 0; hand < 2; hand++)
            suggested[i * 2 + hand] = (XrActionSuggestedBinding){actions[i], path(bindings[i][hand])};
    XrPath profile = path("/interaction_profiles/bytedance/pico_neo3_controller");
    XrInteractionProfileSuggestedBinding suggest = {.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .interactionProfile = profile, .countSuggestedBindings = 8, .suggestedBindings = suggested};
    XR(xrSuggestInteractionProfileBindings(instance, &suggest));
    XrSessionActionSetsAttachInfo attach = {.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1, .actionSets = &set};
    XR(xrAttachSessionActionSets(session, &attach));
    bool running = false, focused = false;
    double deadline = now() + 15;
    while (!focused && now() < deadline) {
        XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = xrPollEvent(instance, &event);
        XR(result);
        if (result == XR_SUCCESS && event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
            ((XrEventDataSessionStateChanged *)&event)->state == XR_SESSION_STATE_READY) {
            XrSessionBeginInfo begin = {.type = XR_TYPE_SESSION_BEGIN_INFO,
                .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
            XR(xrBeginSession(session, &begin));
            running = true;
        }
        if (result == XR_SUCCESS && event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
            ((XrEventDataSessionStateChanged *)&event)->state == XR_SESSION_STATE_FOCUSED)
            focused = true;
        if (result == XR_EVENT_UNAVAILABLE) {
            struct timespec delay = {.tv_nsec = 1000000};
            nanosleep(&delay, NULL);
        }
    }
    assert(running && focused);
    const char *phases[] = {"left", "right", "release", "malformed"};
    for (unsigned step = 0; step < 4; step++) {
        phase(phases[step]);
        unsigned matches = 0;
        deadline = now() + 15;
        while (matches < 3 && now() < deadline) {
            struct timespec delay = {.tv_nsec = 20000000};
            nanosleep(&delay, NULL);
            XrActiveActionSet active = {.actionSet = set};
            XrActionsSyncInfo sync = {.type = XR_TYPE_ACTIONS_SYNC_INFO,
                .countActiveActionSets = 1, .activeActionSets = &active};
            XR(xrSyncActions(session, &sync));
            bool matched = true;
            for (unsigned hand = 0; hand < 2; hand++) {
                XrInteractionProfileState profile_state = {.type = XR_TYPE_INTERACTION_PROFILE_STATE};
                XR(xrGetCurrentInteractionProfile(session, hands[hand], &profile_state));
                assert(profile_state.interactionProfile == profile);
                XrActionStateGetInfo query = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .subactionPath = hands[hand]};
                for (unsigned i = 0; i < 2; i++) {
                    query.action = actions[i];
                    XrActionStateBoolean state = {.type = XR_TYPE_ACTION_STATE_BOOLEAN};
                    XR(xrGetActionStateBoolean(session, &query, &state));
                    matched &= state.isActive == (step < 3) && state.currentState == (step == hand);
                    if (state.isActive && state.currentState)
                        assert(state.lastChangeTime > 0);
                }
                query.action = actions[2];
                XrActionStateFloat analog = {.type = XR_TYPE_ACTION_STATE_FLOAT};
                XR(xrGetActionStateFloat(session, &query, &analog));
                assert(!analog.isActive && analog.currentState == 0);
                query.action = actions[3];
                XrActionStatePose pose = {.type = XR_TYPE_ACTION_STATE_POSE};
                XR(xrGetActionStatePose(session, &query, &pose));
                assert(!pose.isActive);
            }
            matches = matched ? matches + 1 : 0;
        }
        assert(matches == 3);
        printf("PICO_OPENXR_PHASE_PASS %s\n", phases[step]);
        fflush(stdout);
    }
    XR(xrDestroySession(session));
    XR(xrDestroyActionSet(set));
    XR(xrDestroyInstance(instance));
    puts("PICO_OPENXR_INPUT_PASS both_hands=2 phases=4 analog_inactive=1 pose_inactive=1");
    return 0;
}
