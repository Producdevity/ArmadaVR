#ifdef NDEBUG
#error This test requires assertions
#endif
#define XR_USE_TIMESPEC
#include <ctime>
#define XR_USE_GRAPHICS_API_VULKAN
#include "xr-renderer.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

static void check(XrResult value) {
    if (XR_FAILED(value)) throw std::runtime_error("OpenXR error " + std::to_string(value));
}
static double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static std::string read(const char *path) {
    std::ifstream file(path); std::string value; std::getline(file, value); return value;
}
static void write(const char *path, const std::string &value) {
    std::ofstream file(path); file << value; file.close(); assert(file);
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        std::ifstream board("/proc/device-tree/compatible");
        std::string identity; std::getline(board, identity, '\0');
        assert(identity == "linux,dummy-virt");
        assert(read("/proc/cmdline").find("reframed.ssc_test=1") != std::string::npos);
        const char *extensions[] = {"XR_KHR_vulkan_enable2", "XR_BD_controller_interaction", "XR_KHR_convert_timespec_time"};
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(create.applicationInfo.applicationName, "Pico stereo framebuffer test");
        create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        create.enabledExtensionCount = 3; create.enabledExtensionNames = extensions;
        XrInstance instance; check(xrCreateInstance(&create, &instance));
        PFN_xrConvertTimespecTimeToTimeKHR convert_time = nullptr;
        check(xrGetInstanceProcAddr(instance, "xrConvertTimespecTimeToTimeKHR", reinterpret_cast<PFN_xrVoidFunction *>(&convert_time)));
        auto path = [&](const char *text) { XrPath result; check(xrStringToPath(instance, text, &result)); return result; };
        XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO}; get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system; check(xrGetSystem(instance, &get, &system));
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        check(xrGetSystemProperties(instance, system, &properties));
        assert(std::strstr(properties.systemName, "Pico Neo3 HMD"));
        assert(properties.trackingProperties.orientationTracking && !properties.trackingProperties.positionTracking);
        {
            XrRenderer renderer; renderer.initialize(instance, system);
            XrSessionCreateInfo session_create{XR_TYPE_SESSION_CREATE_INFO};
            session_create.systemId = system; session_create.next = &renderer.binding;
            XrSession session; check(xrCreateSession(instance, &session_create, &session));
            renderer.create_swapchains(session);
            XrActionSetCreateInfo set_create{XR_TYPE_ACTION_SET_CREATE_INFO};
            std::strcpy(set_create.actionSetName, "controllers"); std::strcpy(set_create.localizedActionSetName, "Controllers");
            XrActionSet set; check(xrCreateActionSet(instance, &set_create, &set));
            const XrPath hands[] = {path("/user/hand/left"), path("/user/hand/right")};
            XrActionCreateInfo action_create{XR_TYPE_ACTION_CREATE_INFO};
            action_create.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT; action_create.countSubactionPaths = 2;
            action_create.subactionPaths = hands;
            std::strcpy(action_create.actionName, "button"); std::strcpy(action_create.localizedActionName, "Button");
            XrAction button; check(xrCreateAction(set, &action_create, &button));
            XrActionSuggestedBinding bindings[] = {{button, path("/user/hand/left/input/x/click")},
                                                   {button, path("/user/hand/right/input/a/click")}};
            XrPath profile = path("/interaction_profiles/bytedance/pico_neo3_controller");
            XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggest.interactionProfile = profile; suggest.countSuggestedBindings = 2; suggest.suggestedBindings = bindings;
            check(xrSuggestInteractionProfileBindings(instance, &suggest));
            XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            attach.countActionSets = 1; attach.actionSets = &set; check(xrAttachSessionActionSets(session, &attach));
            XrReferenceSpaceCreateInfo space_create{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            space_create.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; space_create.poseInReferenceSpace.orientation.w = 1;
            XrSpace view; check(xrCreateReferenceSpace(session, &space_create, &view));
            space_create.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            XrSpace local; check(xrCreateReferenceSpace(session, &space_create, &local));
            XrQuaternionf initial{}; bool have_initial = false; double motion = 0;
            const char *phases[] = {"release", "left", "right", "release"};
            unsigned step = 0, matches = 0, attempts = 0; bool running = false, focused = false;
            double deadline = now() + 120;
            write("/controller-phase", phases[0]); write("/render-ready", "starting");
            while (step < 4 && now() < deadline) {
                XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
                XrResult result = xrPollEvent(instance, &event); check(result);
                if (result == XR_SUCCESS && event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    auto state = reinterpret_cast<XrEventDataSessionStateChanged *>(&event)->state;
                    if (state == XR_SESSION_STATE_READY) {
                        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        check(xrBeginSession(session, &begin)); running = true;
                    }
                    focused = state == XR_SESSION_STATE_FOCUSED;
                    assert(state != XR_SESSION_STATE_LOSS_PENDING && state != XR_SESSION_STATE_EXITING);
                }
                if (!running) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
                auto frame = renderer.begin(session);
                std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
                XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
                locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                locate.displayTime = frame.predictedDisplayTime; locate.space = view;
                XrViewState state{XR_TYPE_VIEW_STATE}; uint32_t count;
                check(xrLocateViews(session, &locate, &state, 2, &count, views.data())); assert(count == 2);
                frame.shouldRender = frame.shouldRender && (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
                std::array<bool, 2> pressed{};
                bool matched = focused && frame.shouldRender && (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
                XrActiveActionSet active{set, XR_NULL_PATH};
                XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO}; sync.countActiveActionSets = 1; sync.activeActionSets = &active;
                check(xrSyncActions(session, &sync));
                for (unsigned hand = 0; hand < 2; hand++) {
                    XrActionStateGetInfo query{XR_TYPE_ACTION_STATE_GET_INFO}; query.action = button; query.subactionPath = hands[hand];
                    XrActionStateBoolean input{XR_TYPE_ACTION_STATE_BOOLEAN}; check(xrGetActionStateBoolean(session, &query, &input));
                    pressed[hand] = input.isActive && input.currentState;
                    matched &= input.isActive && pressed[hand] == (step == hand + 1);
                    if (frame.shouldRender) assert(std::fabs(views[hand].pose.position.x - (hand ? .032f : -.032f)) < .0001f);
                    if (frame.shouldRender) assert(std::fabs(views[hand].fov.angleLeft + .7f) < .0001f);
                }
                if (++attempts % 100 == 0) {
                    struct timespec ts{}; assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
                    XrTime current_time; check(convert_time(instance, &ts, &current_time));
                    XrSpaceLocation current{XR_TYPE_SPACE_LOCATION};
                    check(xrLocateSpace(view, local, current_time, &current));
                    std::printf("PICO_RENDER_STATE attempts=%u valid=%llu current=%llu lead_ms=%.3f render=%d focused=%d left=%d right=%d matched=%d\n", attempts,
                        static_cast<unsigned long long>(state.viewStateFlags), static_cast<unsigned long long>(current.locationFlags),
                        (frame.predictedDisplayTime-current_time)/1e6, frame.shouldRender, focused, pressed[0], pressed[1], matched);
                }
                XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
                check(xrLocateSpace(view, local, frame.predictedDisplayTime, &head));
                if (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
                    const auto q = head.pose.orientation;
                    if (!have_initial) { initial = q; have_initial = true; }
                    double dot = q.x * initial.x + q.y * initial.y + q.z * initial.z + q.w * initial.w;
                    assert(std::isfinite(dot));
                    motion = std::max(motion, 1 - std::fabs(dot));
                }
                renderer.end(session, view, frame, views, pressed);
                matches = matched ? matches + 1 : 0;
                if (matches >= 8) write("/render-ready", std::to_string(step));
                if (matches >= 8 && read("/render-next") == std::to_string(step)) {
                    std::printf("PICO_RENDER_PHASE_PASS step=%u frames=%u\n", step, renderer.submitted); std::fflush(stdout);
                    step++; matches = 0;
                    if (step < 4) { write("/controller-phase", phases[step]); write("/render-ready", "waiting"); }
                }
            }
            assert(step == 4 && renderer.submitted >= 32);
            assert(renderer.pressed_frames[0] >= 8 && renderer.pressed_frames[1] >= 8);
            assert(have_initial && motion > 0.005);
            std::printf("PICO_HMD_RENDER_MOTION=%.9f\n", motion);
            renderer.release_swapchains();
            check(xrDestroySpace(view)); check(xrDestroySpace(local)); check(xrDestroySession(session)); check(xrDestroyActionSet(set));
            std::printf("PICO_OPENXR_RENDER_PASS frames=%u left=%u right=%u\n", renderer.submitted,
                        renderer.pressed_frames[0], renderer.pressed_frames[1]);
        }
        check(xrDestroyInstance(instance));
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
