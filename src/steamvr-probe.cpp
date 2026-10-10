#include <vulkan/vulkan.h>
#include <openvr.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <unistd.h>

static void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

static void vk_check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL: %s VkResult=%d\n", operation, result);
        std::exit(1);
    }
}
#define VK_CHECK(call) vk_check(call, #call)

static uint32_t extensions(char *text, const char **names, uint32_t capacity) {
    uint32_t count = 0;
    char *state = nullptr;
    for (char *name = strtok_r(text, " ", &state); name; name = strtok_r(nullptr, " ", &state)) {
        require(count < capacity, "Too many Vulkan extensions");
        names[count++] = name;
    }
    return count;
}

static void input_check(vr::EVRInputError error, const char *operation) {
    if (error != vr::VRInputError_None) {
        std::fprintf(stderr, "FAIL: %s input error=%d\n", operation, error);
        std::exit(1);
    }
}
#define INPUT_CHECK(call) input_check(call, #call)

static bool controllers(vr::IVRSystem *system, vr::IVRDebug *debug, vr::IVRInput *input, const char *executable) {
    auto manifest = std::filesystem::canonical(executable).parent_path() / "actions.json";
    INPUT_CHECK(input->SetActionManifestPath(manifest.c_str()));
    vr::VRActiveActionSet_t active{};
    INPUT_CHECK(input->GetActionSetHandle("/actions/probe", &active.ulActionSet));
    vr::VRActionHandle_t trigger, grip, pull, haptic, menu, pad_click, pad_touch, pad_position;
    INPUT_CHECK(input->GetActionHandle("/actions/probe/in/trigger", &trigger));
    INPUT_CHECK(input->GetActionHandle("/actions/probe/in/grip", &grip));
    INPUT_CHECK(input->GetActionHandle("/actions/probe/in/pull", &pull));
    INPUT_CHECK(input->GetActionHandle("/actions/probe/out/haptic", &haptic));
    INPUT_CHECK(input->GetActionHandle("/actions/probe/in/menu", &menu));
    INPUT_CHECK(input->GetActionHandle("/actions/probe/in/pad_click", &pad_click));
    INPUT_CHECK(input->GetActionHandle("/actions/probe/in/pad_touch", &pad_touch));
    INPUT_CHECK(input->GetActionHandle("/actions/probe/in/pad_position", &pad_position));
    bool passed = true;
    for (unsigned hand = 0; hand < 2; ++hand) {
        vr::VRInputValueHandle_t source;
        INPUT_CHECK(input->GetInputSourceHandle(hand ? "/user/hand/right" : "/user/hand/left", &source));
        auto index = vr::k_unTrackedDeviceIndexInvalid;
        for (unsigned device = 1; device < vr::k_unMaxTrackedDeviceCount; ++device) {
            if (system->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller ||
                !system->IsTrackedDeviceConnected(device)) continue;
            char serial[128]{};
            system->GetStringTrackedDeviceProperty(device, vr::Prop_SerialNumber_String, serial, sizeof(serial));
            if (std::strcmp(serial, hand ? "armada-vr-right" : "armada-vr-left")) continue;
            if (index != vr::k_unTrackedDeviceIndexInvalid) return false;
            index = device;
        }
        if (index == vr::k_unTrackedDeviceIndexInvalid) { std::puts("FAIL: missing virtual controller"); return false; }
        char tracking[128] = {};
        system->GetStringTrackedDeviceProperty(index, vr::Prop_TrackingSystemName_String, tracking, sizeof(tracking));
        if (std::strcmp(tracking, "armada_virtual")) { std::puts("FAIL: controller is not simulated"); return false; }
        auto buttons = [&](bool down) {
            INPUT_CHECK(input->UpdateActionState(&active, sizeof(active), 1));
            vr::InputDigitalActionData_t trigger_data{}, grip_data{};
            vr::InputAnalogActionData_t pull_data{};
            INPUT_CHECK(input->GetDigitalActionData(trigger, &trigger_data, sizeof(trigger_data), source));
            INPUT_CHECK(input->GetDigitalActionData(grip, &grip_data, sizeof(grip_data), source));
            INPUT_CHECK(input->GetAnalogActionData(pull, &pull_data, sizeof(pull_data), source));
            for (auto action : {menu, pad_click, pad_touch}) {
                vr::InputDigitalActionData_t data{};
                INPUT_CHECK(input->GetDigitalActionData(action, &data, sizeof(data), source));
                if (!data.bActive || data.bState != down) return false;
                vr::InputOriginInfo_t detail{};
                INPUT_CHECK(input->GetOriginTrackedDeviceInfo(data.activeOrigin, &detail, sizeof(detail)));
                if (detail.devicePath != source || detail.trackedDeviceIndex != index) return false;
            }
            vr::InputAnalogActionData_t position{};
            INPUT_CHECK(input->GetAnalogActionData(pad_position, &position, sizeof(position), source));
            if (!position.bActive || std::fabs(position.x - (down ? 1.f : 0.f)) >= 0.001f ||
                std::fabs(position.y - (down ? -0.5f : 0.f)) >= 0.001f) return false;
            vr::InputOriginInfo_t origin{};
            if (trigger_data.bActive)
                INPUT_CHECK(input->GetOriginTrackedDeviceInfo(trigger_data.activeOrigin, &origin, sizeof(origin)));
            static unsigned samples = 0;
            if (!(samples++ % 100))
                std::printf("input_available=%d active=%d/%d/%d state=%d/%d/%.3f\n",
                            system->IsInputAvailable(), trigger_data.bActive, grip_data.bActive, pull_data.bActive,
                            trigger_data.bState, grip_data.bState, pull_data.x);
            return trigger_data.bActive && grip_data.bActive && pull_data.bActive &&
                   origin.devicePath == source && origin.trackedDeviceIndex == index &&
                   trigger_data.bState == down && grip_data.bState == down &&
                   std::fabs(pull_data.x - (down ? 1.f : 0.f)) < 0.001f;
        };
        auto request = [&](const char *command, char *response, uint32_t length) {
            debug->DriverDebugRequest(index, command, response, length);
            return !std::strcmp(response, "ok");
        };
        char response[256] = {};
        debug->DriverDebugRequest(index, "pose nan 1 0 0", response, sizeof(response));
        passed &= !std::strncmp(response, "error:", 6);
        for (const char *invalid : {"trackpad nan 0 0 0", "trackpad 1.1 0 0 0", "trackpad 0 0 2 1", "menu 2"}) {
            debug->DriverDebugRequest(index, invalid, response, sizeof(response));
            passed &= !std::strncmp(response, "error:", 6);
        }
        passed &= request("pose 0.5 1.4 -0.7 30", response, sizeof(response));
        passed &= request("buttons 1 1 0", response, sizeof(response));
        passed &= request("trackpad 1 -0.5 1 1", response, sizeof(response));
        passed &= request("menu 1", response, sizeof(response));
        bool pose_seen = false, pressed = false;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
            system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0, poses, vr::k_unMaxTrackedDeviceCount);
            pose_seen = poses[index].bPoseIsValid && std::fabs(poses[index].mDeviceToAbsoluteTracking.m[0][3] - 0.5f) < 0.001f &&
                        std::fabs(poses[index].mDeviceToAbsoluteTracking.m[1][3] - 1.4f) < 0.001f;
            pressed = buttons(true);
            if (pose_seen && pressed) break;
            usleep(20000);
        }
        unsigned before = 0, after = 0;
        debug->DriverDebugRequest(index, "status", response, sizeof(response));
        passed &= std::sscanf(response, "haptics=%u", &before) == 1;
        INPUT_CHECK(input->TriggerHapticVibrationAction(haptic, 0, 0.05f, 100.f, 0.5f, source));
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            debug->DriverDebugRequest(index, "status", response, sizeof(response));
            if (std::sscanf(response, "haptics=%u", &after) == 1 && after > before) break;
            usleep(20000);
        }
        passed &= request("buttons 0 0 0", response, sizeof(response));
        passed &= request("trackpad 0 0 0 0", response, sizeof(response));
        passed &= request("menu 0", response, sizeof(response));
        passed &= request(hand ? "pose 0.25 1.2 -0.5 0" : "pose -0.25 1.2 -0.5 0", response, sizeof(response));
        bool released = false;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            released = buttons(false);
            if (released) break;
            usleep(20000);
        }
        std::printf("controller=%u pose_changed=%d buttons_menu_trackpad_pressed=%d released=%d haptics=%u->%u\n",
                    hand, pose_seen, pressed, released, before, after);
        passed &= pose_seen && pressed && released && after > before;
    }
    return passed;
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    alarm(180);
    require(argc == 3, "Usage: steamvr-probe LIBOPENVR_API --connect|--frames|--setup-room|--controllers|--dashboard");
    bool frames = !std::strcmp(argv[2], "--frames");
    bool setup = !std::strcmp(argv[2], "--setup-room");
    bool input = !std::strcmp(argv[2], "--controllers");
    bool dashboard = !std::strcmp(argv[2], "--dashboard");
    require(frames || setup || input || dashboard || !std::strcmp(argv[2], "--connect"), "Unknown probe mode");
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    auto init = reinterpret_cast<uint32_t (*)(vr::EVRInitError *, vr::EVRApplicationType, const char *)>(
        dlsym(library, "VR_InitInternal2"));
    auto get = reinterpret_cast<void *(*)(const char *, vr::EVRInitError *)>(dlsym(library, "VR_GetGenericInterface"));
    auto shutdown = reinterpret_cast<void (*)()>(dlsym(library, "VR_ShutdownInternal"));
    require(init && get && shutdown, "Missing OpenVR exports");
    vr::EVRInitError error = vr::VRInitError_None;
    uint32_t token = init(&error, frames ? vr::VRApplication_Scene : input ? vr::VRApplication_Overlay : vr::VRApplication_Background, nullptr);
    std::printf("init token=%u error=%d\n", token, error);
    require(token && error == vr::VRInitError_None, "OpenVR initialization failed");
    auto system = static_cast<vr::IVRSystem *>(get(vr::IVRSystem_Version, &error));
    require(system && error == vr::VRInitError_None, "No IVRSystem");
    vr::IVRCompositor *compositor = nullptr;
    if (!input && !setup) {
        compositor = static_cast<vr::IVRCompositor *>(get(vr::IVRCompositor_Version, &error));
        require(compositor && error == vr::VRInitError_None, "No IVRCompositor");
    }
    char tracking[128] = {};
    vr::ETrackedPropertyError property_error;
    system->GetStringTrackedDeviceProperty(0, vr::Prop_TrackingSystemName_String, tracking, sizeof(tracking), &property_error);
    require(property_error == vr::TrackedProp_Success, "Cannot identify tracking driver");
    std::printf("tracking=%s compositor_connected=%s\n", tracking, compositor ? "true" : "false");
    char model[128] = {};
    system->GetStringTrackedDeviceProperty(0, vr::Prop_ModelNumber_String, model, sizeof(model));
    require(!std::strcmp(tracking, "null") || (!std::strcmp(tracking, "armada_virtual") &&
        !std::strcmp(model, "Armada VR virtual headset")), "This probe requires a simulated headset");
    if (dashboard) {
        auto overlay = static_cast<vr::IVROverlay *>(get(vr::IVROverlay_Version, &error));
        require(overlay && error == vr::VRInitError_None, "No IVROverlay");
        overlay->ShowDashboard("");
        bool visible = false;
        for (unsigned attempt = 0; attempt < 600; ++attempt) {
            visible = overlay->IsDashboardVisible();
            if (visible) break;
            usleep(100000);
        }
        shutdown();
        dlclose(library);
        require(visible, "SteamVR dashboard did not become visible");
        std::puts("PASS: dashboard visibility; rendered content still requires capture verification");
        return 0;
    }
    if (input) {
        auto debug = static_cast<vr::IVRDebug *>(get(vr::IVRDebug_Version, &error));
        require(debug && error == vr::VRInitError_None, "No IVRDebug");
        auto vr_input = static_cast<vr::IVRInput *>(get(vr::IVRInput_Version, &error));
        require(vr_input && error == vr::VRInitError_None, "No IVRInput");
        if (!std::strcmp(tracking, "armada_virtual")) {
            auto models = static_cast<vr::IVRRenderModels *>(get(vr::IVRRenderModels_Version, &error));
            require(models && error == vr::VRInitError_None, "No IVRRenderModels");
            for (const char *hand : {"/user/hand/left", "/user/hand/right"}) {
                vr::VRInputValueHandle_t path;
                input_check(vr_input->GetInputSourceHandle(hand, &path), "controller pose path");
                for (const char *component : {"tip", "grip"}) {
                    vr::RenderModel_ComponentState_t pose{};
                    vr::RenderModel_ControllerMode_State_t mode{};
                    require(models->GetComponentStateForDevicePath("{armada_virtual}controller", component,
                            path, &mode, &pose), "Missing virtual controller pose component");
                    for (unsigned row = 0; row < 3; ++row)
                        for (unsigned column = 0; column < 4; ++column)
                            require(std::isfinite(pose.mTrackingToComponentLocal.m[row][column]) &&
                                    std::abs(pose.mTrackingToComponentLocal.m[row][column] -
                                             (row == column ? 1.f : 0.f)) < 0.00001f,
                                    "Virtual controller pose is not identity");
                    std::printf("hand=%s component=%s identity=1\n", hand, component);
                }
            }
        }
        bool passed = controllers(system, debug, vr_input, argv[0]);
        shutdown();
        dlclose(library);
        require(passed, "Virtual controller action, pose, or haptic test failed");
        std::puts("PASS: both controllers, pose changes, press/release and haptic delivery");
        return 0;
    }
    if (setup) {
        auto chaperone = static_cast<vr::IVRChaperoneSetup *>(get(vr::IVRChaperoneSetup_Version, &error));
        require(chaperone && error == vr::VRInitError_None, "No IVRChaperoneSetup");
        chaperone->RevertWorkingCopy();
        vr::HmdMatrix34_t identity = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
        chaperone->SetWorkingStandingZeroPoseToRawTrackingPose(&identity);
        chaperone->SetWorkingSeatedZeroPoseToRawTrackingPose(&identity);
        chaperone->SetWorkingPlayAreaSize(2, 2);
        vr::HmdVector2_t corners[4] = {{{-1, -1}}, {{1, -1}}, {{1, 1}}, {{-1, 1}}};
        chaperone->SetWorkingPerimeter(corners, 4);
        require(chaperone->CommitWorkingCopy(vr::EChaperoneConfigFile_Live), "Virtual room commit failed");
        std::puts("PASS: virtual 2 m x 2 m room configured");
    }
    if (!frames) {
        shutdown();
        dlclose(library);
        std::puts("PASS: OpenVR connection");
        return 0;
    }

    char instance_extensions[4096] = {}, device_extensions[4096] = {};
    const char *instance_names[128], *device_names[128];
    uint32_t bytes = compositor->GetVulkanInstanceExtensionsRequired(instance_extensions, sizeof(instance_extensions));
    require(bytes > 0 && bytes <= sizeof(instance_extensions), "Invalid instance extension list");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Armada VR frame probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = extensions(instance_extensions, instance_names, 128);
    instance_info.ppEnabledExtensionNames = instance_names;
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&instance_info, nullptr, &instance));
    uint64_t output_device = 0;
    system->GetOutputDevice(&output_device, vr::TextureType_Vulkan, instance);
    VkPhysicalDevice physical = reinterpret_cast<VkPhysicalDevice>(output_device);
    require(physical, "SteamVR did not identify its Vulkan device");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::printf("GPU=%s\n", properties.deviceName);
    bytes = compositor->GetVulkanDeviceExtensionsRequired(physical, device_extensions, sizeof(device_extensions));
    require(bytes > 0 && bytes <= sizeof(device_extensions), "Invalid device extension list");
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    require(family_count && family_count <= 64, "Unexpected Vulkan queue family count");
    VkQueueFamilyProperties families[64];
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    uint32_t family = 0;
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    require(family < family_count, "No graphics queue");
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = extensions(device_extensions, device_names, 128);
    device_info.ppEnabledExtensionNames = device_names;
    VkDevice device;
    VK_CHECK(vkCreateDevice(physical, &device_info, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkImage images[2];
    VkDeviceMemory memory[2];
    for (unsigned eye = 0; eye < 2; ++eye) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {256, 256, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateImage(device, &info, nullptr, &images[eye]));
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, images[eye], &requirements);
        uint32_t type = 0;
        while (type < memory_properties.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type))) ++type;
        require(type < memory_properties.memoryTypeCount, "No image memory type");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device, &allocation, nullptr, &memory[eye]));
        VK_CHECK(vkBindImageMemory(device, images[eye], memory[eye], 0));
    }
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer command;
    VK_CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    unsigned valid_poses = 0;
    for (unsigned frame = 0; frame < 120; ++frame) {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};
        auto pose_error = compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        if (pose_error) std::fprintf(stderr, "WaitGetPoses error=%d\n", pose_error);
        require(pose_error == vr::VRCompositorError_None, "WaitGetPoses failed");
        if (poses[0].bPoseIsValid && poses[0].bDeviceIsConnected) ++valid_poses;
        VK_CHECK(vkQueueWaitIdle(queue));
        VK_CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(command, &begin));
        for (unsigned eye = 0; eye < 2; ++eye) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.srcAccessMask = frame ? VK_ACCESS_TRANSFER_READ_BIT : 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.image = images[eye];
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            float pulse = (frame / 30) % 2 ? 0.8f : 0.25f;
            VkClearColorValue color{{eye ? 0.02f : pulse, 0.05f, eye ? pulse : 0.02f, 1}};
            vkCmdClearColorImage(command, images[eye], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &color, 1, &barrier.subresourceRange);
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        VK_CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        for (unsigned eye = 0; eye < 2; ++eye) {
            vr::VRVulkanTextureData_t data{reinterpret_cast<uint64_t>(images[eye]), device, physical,
                                          instance, queue, family, 256, 256, VK_FORMAT_R8G8B8A8_UNORM, 1};
            vr::Texture_t texture{&data, vr::TextureType_Vulkan, vr::ColorSpace_Gamma};
            auto result = compositor->Submit(static_cast<vr::EVREye>(eye), &texture);
            if (result) std::fprintf(stderr, "Submit frame=%u eye=%u error=%d\n", frame, eye, result);
            require(result == vr::VRCompositorError_None, "Eye submission failed");
        }
        compositor->PostPresentHandoff();
        if (!(frame % 30)) std::printf("submitted_frame=%u valid_poses=%u\n", frame, valid_poses);
    }
    vr::Compositor_CumulativeStats stats{};
    compositor->GetCumulativeStats(&stats, sizeof(stats));
    std::printf("pid=%u self=%u presents=%u submits=%u valid_poses=%u\n", stats.m_nPid,
                static_cast<unsigned>(getpid()), stats.m_nNumFramePresents, stats.m_nNumFrameSubmits, valid_poses);
    bool passed = stats.m_nPid == static_cast<unsigned>(getpid()) && stats.m_nNumFramePresents > 0 && valid_poses == 120;
    shutdown();
    VK_CHECK(vkDeviceWaitIdle(device));
    vkDestroyCommandPool(device, pool, nullptr);
    for (unsigned eye = 0; eye < 2; ++eye) {
        vkDestroyImage(device, images[eye], nullptr);
        vkFreeMemory(device, memory[eye], nullptr);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    dlclose(library);
    require(passed, "No confirmed application frame presentation or valid virtual poses");
    std::puts("PASS: 120 stereo frame submissions and application presentation");
}
