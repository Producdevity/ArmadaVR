#include <ctime>
#ifdef _WIN32
#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <io.h>
#else
#define XR_USE_TIMESPEC
#endif
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xr-renderer.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void check(XrResult result, const char *call) {
    if (XR_FAILED(result)) {
        throw std::runtime_error(std::string(call) + " failed: " + std::to_string(result));
    }
}
#define XR(call) check((call), #call)

struct Runtime {
    XrRenderer renderer;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    XrActionSet actions = XR_NULL_HANDLE;
    std::array<XrSpace, 2> hands{};
    ~Runtime() {
        renderer.release_swapchains();
        for (auto hand : hands) if (hand) xrDestroySpace(hand);
        if (local) xrDestroySpace(local);
        if (session) xrDestroySession(session);
        if (actions) xrDestroyActionSet(actions);
        if (instance) xrDestroyInstance(instance);
    }
    XrPath path(const char *name) const {
        XrPath value;
        XR(xrStringToPath(instance, name, &value));
        return value;
    }
};

int main(int argc, char **argv) {
#ifdef _WIN32
    // Wine console output is not reliably inherited across the Proton launcher.
    if (const char *output = std::getenv("ARMADA_VR_PROBE_OUTPUT")) {
        if (!std::freopen(output, "w", stdout) || _dup2(_fileno(stdout), _fileno(stderr))) return 2;
        setvbuf(stdout, nullptr, _IOLBF, 0);
    }
#endif
    try {
        const bool remote = argc >= 2 && std::strcmp(argv[1], "--remote-input") == 0;
        const bool render = argc == 3 && remote && std::strcmp(argv[2], "--render") == 0;
        if (argc != 1 && !(argc == 2 && remote) && !render)
            throw std::runtime_error("Usage: xr-probe [--remote-input [--render]]");
        Runtime r;
#ifdef _WIN32
        const char *timeExtension = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
        const char *timeFunction = "xrConvertWin32PerformanceCounterToTimeKHR";
        PFN_xrConvertWin32PerformanceCounterToTimeKHR convertTime;
#else
        const char *timeExtension = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
        const char *timeFunction = "xrConvertTimespecTimeToTimeKHR";
        PFN_xrConvertTimespecTimeToTimeKHR convertTime;
#endif
        const char *extensions[] = {render ? XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME : XR_MND_HEADLESS_EXTENSION_NAME,
                                    timeExtension};
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(create.applicationInfo.applicationName, "Armada VR probe");
        create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        create.enabledExtensionCount = 2;
        create.enabledExtensionNames = extensions;
        XR(xrCreateInstance(&create, &r.instance));
        XR(xrGetInstanceProcAddr(r.instance, timeFunction,
            reinterpret_cast<PFN_xrVoidFunction *>(&convertTime)));
        XrInstanceProperties runtime{XR_TYPE_INSTANCE_PROPERTIES};
        XR(xrGetInstanceProperties(r.instance, &runtime));
        XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};
        get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system;
        XR(xrGetSystem(r.instance, &get, &system));
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
        XR(xrGetSystemProperties(r.instance, system, &props));
        std::printf("Runtime: %s; HMD: %s\n", runtime.runtimeName, props.systemName);
        uint32_t viewCount;
        XR(xrEnumerateViewConfigurationViews(r.instance, system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));
        if (viewCount != 2) throw std::runtime_error("Expected a stereo headset");

        XrSessionCreateInfo session{XR_TYPE_SESSION_CREATE_INFO};
        session.systemId = system;
        if (render) {
            r.renderer.initialize(r.instance, system);
            session.next = &r.renderer.binding;
        }
        XR(xrCreateSession(r.instance, &session, &r.session));
        if (render) r.renderer.create_swapchains(r.session);
        XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        space.poseInReferenceSpace.orientation.w = 1;
        XR(xrCreateReferenceSpace(r.session, &space, &r.local));

        XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(set.actionSetName, "tracking");
        std::strcpy(set.localizedActionSetName, "Tracking");
        XR(xrCreateActionSet(r.instance, &set, &r.actions));
        std::array<XrPath, 2> handPaths{r.path("/user/hand/left"), r.path("/user/hand/right")};
        XrActionCreateInfo action{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(action.actionName, "grip");
        std::strcpy(action.localizedActionName, "Grip pose");
        action.actionType = XR_ACTION_TYPE_POSE_INPUT;
        action.countSubactionPaths = handPaths.size();
        action.subactionPaths = handPaths.data();
        XrAction grip;
        XR(xrCreateAction(r.actions, &action, &grip));
        std::vector<XrActionSuggestedBinding> bindings{
            {grip, r.path("/user/hand/left/input/grip/pose")},
            {grip, r.path("/user/hand/right/input/grip/pose")}
        };
        XrAction select = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, vibration = XR_NULL_HANDLE;
        if (remote) {
            auto add = [&](const char *name, XrActionType type, const char *binding, XrAction *target) {
                std::strcpy(action.actionName, name);
                std::strcpy(action.localizedActionName, name);
                action.actionType = type;
                XR(xrCreateAction(r.actions, &action, target));
                for (const char *hand : {"left", "right"}) {
                    std::string path = std::string("/user/hand/") + hand + binding;
                    bindings.push_back({*target, r.path(path.c_str())});
                }
            };
            add("select", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/trigger/click", &select);
            add("trigger", XR_ACTION_TYPE_FLOAT_INPUT, "/input/trigger/value", &trigger);
            add("vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, "/output/haptic", &vibration);
        }
        XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggest.interactionProfile = r.path(remote ? "/interaction_profiles/valve/index_controller" :
                                                    "/interaction_profiles/khr/simple_controller");
        suggest.countSuggestedBindings = bindings.size();
        suggest.suggestedBindings = bindings.data();
        XR(xrSuggestInteractionProfileBindings(r.instance, &suggest));
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &r.actions;
        XR(xrAttachSessionActionSets(r.session, &attach));
        for (size_t i = 0; i < r.hands.size(); ++i) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = grip;
            info.subactionPath = handPaths[i];
            info.poseInActionSpace.orientation.w = 1;
            XR(xrCreateActionSpace(r.session, &info, &r.hands[i]));
        }

        bool running = false, stopping = false, ready = false;
        unsigned samples = 0, validViews = 0;
        std::array<unsigned, 2> validHands{};
        std::array<bool, 2> pressed{}, released{}, disconnected{}, reconnected{}, hapticAccepted{};
        std::array<float, 2> triggerMaximum{};
        const unsigned requiredSamples = remote ? 600 : 120;
        XrTime lastTime = 0;
        float firstY = 0, maxMotion = 0, separation = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(render ? 40 : 20);
        while (std::chrono::steady_clock::now() < deadline) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            XrResult result;
            while ((result = xrPollEvent(r.instance, &event)) == XR_SUCCESS) {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    auto state = reinterpret_cast<XrEventDataSessionStateChanged *>(&event)->state;
                    std::printf("Session state: %d\n", state);
                    if (state == XR_SESSION_STATE_READY) {
                        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        XR(xrBeginSession(r.session, &begin));
                        running = true;
                    } else if (state == XR_SESSION_STATE_STOPPING) {
                        XR(xrEndSession(r.session));
                        running = false;
                        stopping = true;
                    } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                        stopping = true;
                    } else if (state == XR_SESSION_STATE_FOCUSED && remote && !ready) {
                        std::puts("ARMADA_VR_INPUT_READY");
                        std::fflush(stdout);
                        ready = true;
                    }
                }
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }
            XR(result);
            if (stopping) break;
            if (!running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            if (render) frame = r.renderer.begin(r.session);
            else std::this_thread::sleep_for(std::chrono::milliseconds(16));
#ifdef _WIN32
            LARGE_INTEGER now{};
            if (!QueryPerformanceCounter(&now)) throw std::runtime_error("QueryPerformanceCounter failed");
#else
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now)) throw std::runtime_error("clock_gettime failed");
#endif
            XrTime sampleTime;
            XR(convertTime(r.instance, &now, &sampleTime));
            if (render) sampleTime = frame.predictedDisplayTime;
            if (sampleTime <= lastTime) throw std::runtime_error("Non-monotonic sample time");
            lastTime = sampleTime;
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = sampleTime;
            locate.space = r.local;
            XrViewState state{XR_TYPE_VIEW_STATE};
            std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
            XR(xrLocateViews(r.session, &locate, &state, views.size(), &viewCount, views.data()));
            const auto flags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
            if ((state.viewStateFlags & flags) == flags && viewCount == 2) {
                ++validViews;
                auto a = views[0].pose.position, b = views[1].pose.position;
                separation = std::sqrt((a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y) + (a.z-b.z)*(a.z-b.z));
                if (!samples) firstY = a.y;
                maxMotion = std::fmax(maxMotion, std::fabs(a.y-firstY));
            }
            XrActiveActionSet active{r.actions, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            XR(xrSyncActions(r.session, &sync));
            std::array<bool, 2> currentPressed{};
            for (size_t i = 0; i < r.hands.size(); ++i) {
                XrSpaceLocation pose{XR_TYPE_SPACE_LOCATION};
                XR(xrLocateSpace(r.hands[i], r.local, sampleTime, &pose));
                const auto handFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                if ((pose.locationFlags & handFlags) == handFlags) ++validHands[i];
                if (remote) {
                    XrActionStateGetInfo getAction{XR_TYPE_ACTION_STATE_GET_INFO};
                    getAction.action = select;
                    getAction.subactionPath = handPaths[i];
                    XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
                    XR(xrGetActionStateBoolean(r.session, &getAction, &button));
                    currentPressed[i] = button.isActive && button.currentState;
                    if (button.isActive) {
                        if (disconnected[i]) reconnected[i] = true;
                        if (button.currentState && button.lastChangeTime > 0) pressed[i] = true;
                        if (pressed[i] && !button.currentState && button.changedSinceLastSync) released[i] = true;
                    } else if (pressed[i]) disconnected[i] = true;
                    getAction.action = trigger;
                    XrActionStateFloat analog{XR_TYPE_ACTION_STATE_FLOAT};
                    XR(xrGetActionStateFloat(r.session, &getAction, &analog));
                    if (analog.isActive) triggerMaximum[i] = std::fmax(triggerMaximum[i], analog.currentState);
                    if (pressed[i] && !hapticAccepted[i]) {
                        XrHapticActionInfo haptic{XR_TYPE_HAPTIC_ACTION_INFO};
                        haptic.action = vibration;
                        haptic.subactionPath = handPaths[i];
                        XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION};
                        pulse.duration = 10000000;
                        pulse.frequency = XR_FREQUENCY_UNSPECIFIED;
                        pulse.amplitude = 0.5f;
                        XrResult accepted = xrApplyHapticFeedback(r.session, &haptic,
                            reinterpret_cast<XrHapticBaseHeader *>(&pulse));
                        check(accepted, "xrApplyHapticFeedback");
                        hapticAccepted[i] = accepted == XR_SUCCESS;
                        XR(xrStopHapticFeedback(r.session, &haptic));
                    }
                }
            }
            if (render) r.renderer.end(r.session, r.local, frame, views, currentPressed);
            if (++samples == requiredSamples) XR(xrRequestExitSession(r.session));
        }
        std::printf("samples=%u stereo=%u left=%u right=%u ipd=%.4f motion=%.6f stopped=%d\n",
            samples, validViews, validHands[0], validHands[1], separation, maxMotion, stopping);
        const unsigned requiredHands = remote ? 400 : 120;
        if (samples < requiredSamples || validViews < requiredSamples ||
            validHands[0] < requiredHands || validHands[1] < requiredHands ||
            separation < 0.04f || separation > 0.09f || maxMotion < 0.00001f || !stopping) {
            throw std::runtime_error("Simulated headset acceptance checks failed");
        }
        if (remote) {
            for (size_t i = 0; i < r.hands.size(); ++i) {
                std::printf("hand=%zu pressed=%d released=%d disconnected=%d reconnected=%d trigger_max=%.2f haptic_api=%d\n",
                    i, pressed[i], released[i], disconnected[i], reconnected[i], triggerMaximum[i], hapticAccepted[i]);
                if (!pressed[i] || !released[i] || !disconnected[i] || !reconnected[i] ||
                    triggerMaximum[i] < 0.74f || !hapticAccepted[i]) {
                    throw std::runtime_error("Remote controller action acceptance failed");
                }
            }
            std::puts("ARMADA_VR_INPUT_PASS (virtual actions; haptic API only, no output device)");
        }
        if (render) {
            std::printf("submitted_projection_frames=%u left_pressed_frames=%u right_pressed_frames=%u\n",
                r.renderer.submitted, r.renderer.pressed_frames[0], r.renderer.pressed_frames[1]);
            if (r.renderer.submitted < requiredSamples - 5 || !r.renderer.pressed_frames[0] ||
                !r.renderer.pressed_frames[1]) throw std::runtime_error("Rendered input acceptance failed");
            std::puts("ARMADA_VR_RENDERED_INPUT_SUBMITTED (display captures must also pass)");
        } else std::puts("ARMADA_VR_OPENXR_PASS (headless tracking; no rendered layers)");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
