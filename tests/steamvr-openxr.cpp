#define XR_USE_GRAPHICS_API_VULKAN
#include "xr-renderer.h"
#include <openvr.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

static void check(XrResult value) {
    if (XR_FAILED(value)) throw std::runtime_error("OpenXR result=" + std::to_string(value));
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    alarm(90);
    if (argc != 2) return 2;
    try {
        XrInstance instance = XR_NULL_HANDLE;
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(create.applicationInfo.applicationName, "Armada SteamVR OpenXR acceptance");
        create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        const char *extension = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
        create.enabledExtensionCount = 1;
        create.enabledExtensionNames = &extension;
        check(xrCreateInstance(&create, &instance));
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        check(xrGetInstanceProperties(instance, &properties));
        std::printf("runtime=%s\n", properties.runtimeName);
        if (!std::strstr(properties.runtimeName, "SteamVR")) throw std::runtime_error("Expected SteamVR runtime");
        XrSystemGetInfo get_system{XR_TYPE_SYSTEM_GET_INFO};
        get_system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system;
        check(xrGetSystem(instance, &get_system, &system));
        XrRenderer renderer;
        renderer.initialize(instance, system);
        XrSessionCreateInfo create_session{XR_TYPE_SESSION_CREATE_INFO};
        create_session.systemId = system;
        create_session.next = &renderer.binding;
        XrSession session;
        check(xrCreateSession(instance, &create_session, &session));
        renderer.create_swapchains(session);
        XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        reference.poseInReferenceSpace.orientation.w = 1;
        XrSpace space;
        check(xrCreateReferenceSpace(session, &reference, &space));
        void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!library) throw std::runtime_error("OpenVR statistics library unavailable");
        auto generic = reinterpret_cast<void *(*)(const char *, vr::EVRInitError *)>(dlsym(library, "VR_GetGenericInterface"));
        auto init_vr = reinterpret_cast<uint32_t (*)(vr::EVRInitError *, vr::EVRApplicationType, const char *)>(
            dlsym(library, "VR_InitInternal2"));
        auto shutdown_vr = reinterpret_cast<void (*)()>(dlsym(library, "VR_ShutdownInternal"));
        if (!generic || !init_vr || !shutdown_vr) throw std::runtime_error("OpenVR interface export unavailable");
        vr::EVRInitError error;
        if (!init_vr(&error, vr::VRApplication_Background, nullptr) || error)
            throw std::runtime_error("OpenVR statistics initialization failed");
        auto compositor = static_cast<vr::IVRCompositor *>(generic(vr::IVRCompositor_Version, &error));
        if (!compositor || error) throw std::runtime_error("OpenVR compositor statistics unavailable");
        auto vr_system = static_cast<vr::IVRSystem *>(generic(vr::IVRSystem_Version, &error));
        if (!vr_system || error) throw std::runtime_error("OpenVR system unavailable");
        char driver[128]{};
        vr_system->GetStringTrackedDeviceProperty(0, vr::Prop_TrackingSystemName_String, driver, sizeof(driver));
        if (std::strcmp(driver, "null")) throw std::runtime_error("This test requires a virtual null headset");
        auto settings = static_cast<vr::IVRSettings *>(generic(vr::IVRSettings_Version, &error));
        if (!settings || error) throw std::runtime_error("OpenVR settings unavailable");
        settings->SetBool(vr::k_pch_Power_Section, vr::k_pch_Power_PauseCompositorOnStandby_Bool, false);
        auto overlay = static_cast<vr::IVROverlay *>(generic(vr::IVROverlay_Version, &error));
        if (!overlay || error) throw std::runtime_error("Virtual dashboard interface unavailable");
        bool running = false, stopped = false, exit_requested = false;
        unsigned valid = 0;
        vr::Compositor_CumulativeStats before{}, after{};
        compositor->GetCumulativeStats(&before, sizeof(before));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!stopped && std::chrono::steady_clock::now() < deadline) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            XrResult result;
            while ((result = xrPollEvent(instance, &event)) == XR_SUCCESS) {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    auto state = reinterpret_cast<XrEventDataSessionStateChanged *>(&event)->state;
                    std::printf("state=%d\n", state);
                    if (state == XR_SESSION_STATE_READY) {
                        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        check(xrBeginSession(session, &begin));
                        running = true;
                    } else if (state == XR_SESSION_STATE_STOPPING) {
                        check(xrEndSession(session));
                        running = false;
                        stopped = true;
                    }
                }
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }
            check(result);
            if (stopped) break;
            if (!running) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            auto frame = renderer.begin(session);
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = frame.predictedDisplayTime;
            locate.space = space;
            XrViewState state{XR_TYPE_VIEW_STATE};
            std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
            uint32_t count;
            check(xrLocateViews(session, &locate, &state, 2, &count, views.data()));
            auto flags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
            if (count != 2 || (state.viewStateFlags & flags) != flags)
                throw std::runtime_error("Invalid stereo views");
            ++valid;
            renderer.end(session, space, frame, views, {false, false});
            if (!exit_requested && renderer.submitted >= 120) {
                compositor->GetCumulativeStats(&after, sizeof(after));
                check(xrRequestExitSession(session));
                exit_requested = true;
            }
        }
        compositor->GetCumulativeStats(&after, sizeof(after));
        overlay->ShowDashboard("");
        std::printf("before_pid=%u pid=%u self=%u valid=%u layers=%u presents=%u->%u stopped=%d\n",
                    before.m_nPid, after.m_nPid, unsigned(getpid()), valid, renderer.submitted,
                    before.m_nNumFramePresents, after.m_nNumFramePresents, stopped);
        if (!stopped || renderer.submitted < 120 || after.m_nPid != unsigned(getpid()) ||
            !after.m_nNumFramePresents || (before.m_nPid == after.m_nPid &&
            after.m_nNumFramePresents <= before.m_nNumFramePresents))
            throw std::runtime_error("OpenXR compositor presentation acceptance failed");
        renderer.release_swapchains();
        check(xrDestroySpace(space));
        check(xrDestroySession(session));
        check(xrDestroyInstance(instance));
        shutdown_vr();
        dlclose(library);
        std::puts("STEAMVR_OPENXR_PRESENT_PASS (capture verification also required)");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
