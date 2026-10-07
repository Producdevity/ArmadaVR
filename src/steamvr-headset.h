#pragma once

#include <openvr_driver.h>
#include <atomic>
#include <cstdio>
#include <cstring>

class Headset final : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent {
    std::atomic<vr::TrackedDeviceIndex_t> index{vr::k_unTrackedDeviceIndexInvalid};

public:
    vr::EVRInitError Activate(uint32_t device_index) override {
        index = device_index;
        auto properties = vr::VRProperties();
        auto container = properties->TrackedDeviceToPropertyContainer(device_index);
        properties->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "armada_virtual");
        properties->SetStringProperty(container, vr::Prop_ModelNumber_String, "Armada VR virtual headset");
        properties->SetStringProperty(container, vr::Prop_ManufacturerName_String, "Armada VR simulator");
        properties->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, 0.063f);
        properties->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, 30.f);
        properties->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, 1.f / 30.f);
        properties->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 2);
        properties->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, true);
        properties->SetBoolProperty(container, vr::Prop_HasDisplayComponent_Bool, true);
        properties->SetBoolProperty(container, vr::Prop_HasDriverDirectModeComponent_Bool, false);
        properties->SetBoolProperty(container, vr::Prop_HasVirtualDisplayComponent_Bool, false);
        return vr::VRInitError_None;
    }
    void Deactivate() override { index = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void *GetComponent(const char *name) override {
        return !std::strcmp(name, vr::IVRDisplayComponent_Version) ? static_cast<vr::IVRDisplayComponent *>(this) : nullptr;
    }
    void DebugRequest(const char *, char *response, uint32_t size) override {
        if (size) std::snprintf(response, size, "error: fixed virtual headset");
    }
    vr::DriverPose_t GetPose() override {
        vr::DriverPose_t pose{};
        pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = pose.qRotation.w = 1;
        pose.vecPosition[1] = 1.6;
        pose.poseIsValid = pose.deviceIsConnected = true;
        pose.result = vr::TrackingResult_Running_OK;
        return pose;
    }
    void frame() {
        auto device_index = index.load();
        if (device_index != vr::k_unTrackedDeviceIndexInvalid)
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(device_index, GetPose(), sizeof(vr::DriverPose_t));
    }
    void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *width, uint32_t *height) override {
        *x = *y = 0; *width = 1024; *height = 512;
    }
    bool IsDisplayOnDesktop() override { return true; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height) override {
        *width = *height = 512;
    }
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height) override {
        *x = eye == vr::Eye_Left ? 0 : 512; *y = 0; *width = *height = 512;
    }
    void GetProjectionRaw(vr::EVREye, float *left, float *right, float *top, float *bottom) override {
        *left = *top = -1; *right = *bottom = 1;
    }
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float u, float v) override {
        return {{u, v}, {u, v}, {u, v}};
    }
    bool ComputeInverseDistortion(vr::HmdVector2_t *result, vr::EVREye, uint32_t channel, float u, float v) override {
        if (channel > 2) return false;
        result->v[0] = u; result->v[1] = v;
        return true;
    }
};
