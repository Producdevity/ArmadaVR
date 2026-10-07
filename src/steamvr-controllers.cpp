#include <openvr_driver.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

class Controller final : public vr::ITrackedDeviceServerDriver {
    std::mutex mutex;
    const bool left;
    vr::TrackedDeviceIndex_t index = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t container = 0;
    vr::VRInputComponentHandle_t trigger_click = 0, trigger_value = 0, grip = 0, system = 0, haptic = 0;
    vr::VRInputComponentHandle_t menu = 0, trackpad_x = 0, trackpad_y = 0, trackpad_click = 0, trackpad_touch = 0;
    vr::DriverPose_t pose{};
    float trigger = 0;
    bool grip_down = false, system_down = false;
    float pad_x = 0, pad_y = 0;
    bool menu_down = false, pad_clicked = false, pad_touched = false;
    unsigned haptics = 0;

public:
    explicit Controller(bool is_left) : left(is_left) {
        pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = pose.qRotation.w = 1;
        pose.vecPosition[0] = left ? -0.25 : 0.25;
        pose.vecPosition[1] = 1.2;
        pose.vecPosition[2] = -0.5;
        pose.poseIsValid = pose.deviceIsConnected = true;
        pose.result = vr::TrackingResult_Running_OK;
    }

    vr::EVRInitError Activate(uint32_t device_index) override {
        std::lock_guard<std::mutex> lock(mutex);
        index = device_index;
        container = vr::VRProperties()->TrackedDeviceToPropertyContainer(index);
        auto properties = vr::VRProperties();
        properties->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "armada_virtual");
        properties->SetStringProperty(container, vr::Prop_ModelNumber_String,
                                      left ? "Armada VR virtual left controller" : "Armada VR virtual right controller");
        properties->SetStringProperty(container, vr::Prop_ManufacturerName_String, "Armada VR simulator");
        properties->SetStringProperty(container, vr::Prop_ControllerType_String, "vive_controller");
        properties->SetStringProperty(container, vr::Prop_RenderModelName_String, "vr_controller_vive_1_5");
        properties->SetStringProperty(container, vr::Prop_InputProfilePath_String, "{htc}/input/vive_controller_profile.json");
        properties->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32,
                                     left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        properties->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 2);
        properties->SetBoolProperty(container, vr::Prop_DeviceIsWireless_Bool, false);
        auto input = vr::VRDriverInput();
        if (input->CreateBooleanComponent(container, "/input/trigger/click", &trigger_click) ||
            input->CreateScalarComponent(container, "/input/trigger/value", &trigger_value,
                                          vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided) ||
            input->CreateBooleanComponent(container, "/input/grip/click", &grip) ||
            input->CreateBooleanComponent(container, "/input/system/click", &system) ||
            input->CreateBooleanComponent(container, "/input/application_menu/click", &menu) ||
            input->CreateScalarComponent(container, "/input/trackpad/x", &trackpad_x,
                                          vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided) ||
            input->CreateScalarComponent(container, "/input/trackpad/y", &trackpad_y,
                                          vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided) ||
            input->CreateBooleanComponent(container, "/input/trackpad/click", &trackpad_click) ||
            input->CreateBooleanComponent(container, "/input/trackpad/touch", &trackpad_touch) ||
            input->CreateHapticComponent(container, "/output/haptic", &haptic)) {
            index = vr::k_unTrackedDeviceIndexInvalid;
            return vr::VRInitError_Driver_Failed;
        }
        return vr::VRInitError_None;
    }

    void Deactivate() override {
        std::lock_guard<std::mutex> lock(mutex);
        index = vr::k_unTrackedDeviceIndexInvalid;
    }
    void EnterStandby() override {}
    void *GetComponent(const char *) override { return nullptr; }
    vr::DriverPose_t GetPose() override {
        std::lock_guard<std::mutex> lock(mutex);
        return pose;
    }

    void DebugRequest(const char *request, char *response, uint32_t size) override {
        if (!size) return;
        std::lock_guard<std::mutex> lock(mutex);
        const char *result = "error: expected pose x y z yaw, buttons trigger grip system, trackpad x y click touch, menu 0|1, connected 0|1, or status";
        double x, y, z, yaw;
        float value;
        int a, b, end = 0;
        if (std::sscanf(request, "pose %lf %lf %lf %lf%n", &x, &y, &z, &yaw, &end) == 4 &&
            request[end] == '\0' && std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
            std::isfinite(yaw) && std::abs(x) <= 10 && std::abs(y) <= 10 && std::abs(z) <= 10 && std::abs(yaw) <= 360) {
            pose.vecPosition[0] = x; pose.vecPosition[1] = y; pose.vecPosition[2] = z;
            pose.qRotation.w = std::cos(yaw * 3.141592653589793 / 360);
            pose.qRotation.y = std::sin(yaw * 3.141592653589793 / 360);
            result = "ok";
        } else if (std::sscanf(request, "buttons %f %d %d%n", &value, &a, &b, &end) == 3 &&
                   request[end] == '\0' && std::isfinite(value) && value >= 0 && value <= 1 &&
                   (a == 0 || a == 1) && (b == 0 || b == 1)) {
            trigger = value; grip_down = a; system_down = b;
            result = "ok";
        } else if (std::sscanf(request, "connected %d%n", &a, &end) == 1 && request[end] == '\0' && (a == 0 || a == 1)) {
            pose.deviceIsConnected = pose.poseIsValid = a;
            pose.result = a ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;
            result = "ok";
        } else if (std::sscanf(request, "trackpad %lf %lf %d %d%n", &x, &y, &a, &b, &end) == 4 &&
                   request[end] == '\0' && std::isfinite(x) && std::isfinite(y) &&
                   std::abs(x) <= 1 && std::abs(y) <= 1 && (a == 0 || a == 1) && (b == 0 || b == 1)) {
            pad_x = x; pad_y = y; pad_clicked = a; pad_touched = b;
            result = "ok";
        } else if (std::sscanf(request, "menu %d%n", &a, &end) == 1 && request[end] == '\0' && (a == 0 || a == 1)) {
            menu_down = a;
            result = "ok";
        } else if (!std::strcmp(request, "status")) {
            std::snprintf(response, size, "haptics=%u connected=%d trigger=%.3f", haptics, pose.deviceIsConnected, trigger);
            return;
        }
        std::snprintf(response, size, "%s", result);
    }

    void frame() {
        std::lock_guard<std::mutex> lock(mutex);
        if (index == vr::k_unTrackedDeviceIndexInvalid) return;
        auto input = vr::VRDriverInput();
        input->UpdateBooleanComponent(trigger_click, trigger >= 0.75f, 0);
        input->UpdateScalarComponent(trigger_value, trigger, 0);
        input->UpdateBooleanComponent(grip, grip_down, 0);
        input->UpdateBooleanComponent(system, system_down, 0);
        input->UpdateBooleanComponent(menu, menu_down, 0);
        input->UpdateScalarComponent(trackpad_x, pad_x, 0);
        input->UpdateScalarComponent(trackpad_y, pad_y, 0);
        input->UpdateBooleanComponent(trackpad_click, pad_clicked, 0);
        input->UpdateBooleanComponent(trackpad_touch, pad_touched, 0);
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index, pose, sizeof(pose));
    }

    void event(const vr::VREvent_t &event) {
        std::lock_guard<std::mutex> lock(mutex);
        if (event.eventType == vr::VREvent_Input_HapticVibration &&
            event.data.hapticVibration.containerHandle == container &&
            event.data.hapticVibration.componentHandle == haptic) {
            ++haptics;
        }
    }
};

class Provider final : public vr::IServerTrackedDeviceProvider {
    Controller left{true}, right{false};
public:
    vr::EVRInitError Init(vr::IVRDriverContext *context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        char driver[64] = {};
        vr::VRSettings()->GetString("steamvr", "forcedDriver", driver, sizeof(driver));
        if (std::strcmp(driver, "null")) return vr::VRInitError_Driver_Failed;
        if (!vr::VRServerDriverHost()->TrackedDeviceAdded("armada-vr-left", vr::TrackedDeviceClass_Controller, &left) ||
            !vr::VRServerDriverHost()->TrackedDeviceAdded("armada-vr-right", vr::TrackedDeviceClass_Controller, &right))
            return vr::VRInitError_Driver_Failed;
        vr::VRDriverLog()->Log("Armada VR simulated controllers registered; no hardware transport");
        return vr::VRInitError_None;
    }
    void Cleanup() override { left.Deactivate(); right.Deactivate(); VR_CLEANUP_SERVER_DRIVER_CONTEXT(); }
    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override {
        vr::VREvent_t event;
        while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) { left.event(event); right.event(event); }
        left.frame(); right.frame();
    }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
};

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name, int *error) {
    static Provider provider;
    if (!std::strcmp(name, vr::IServerTrackedDeviceProvider_Version)) return &provider;
    if (error) *error = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
