#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
    fprintf(stderr, "%s: VkResult %d\n", #call, r); exit(1); } } while (0)
#define REQUIRE(test) do { if (!(test)) { \
    fprintf(stderr, "%s failed (errno %d)\n", #test, errno); exit(1); } } while (0)
#define BUFFER_SIZE (4 * 1024 * 1024)
#define WAIT_NS UINT64_C(10000000000)

struct context {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t *mapped;
    VkSemaphore forward, back;
    VkBool32 timeline;
    VkBool32 import_only;
};

struct allocation {
    VkDeviceSize size;
    uint32_t memory_type;
    uint8_t device_uuid[VK_UUID_SIZE];
};

static void init(struct context *c)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Armada VR external synchronization test", .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&instance, NULL, &c->instance));
    uint32_t count = 1;
    CHECK(vkEnumeratePhysicalDevices(c->instance, &count, &c->gpu));
    REQUIRE(count == 1);
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->gpu, &family_count, NULL);
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    REQUIRE(families);
    vkGetPhysicalDeviceQueueFamilyProperties(c->gpu, &family_count, families);
    uint32_t family = 0;
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    REQUIRE(family < family_count);
    free(families);
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceTimelineSemaphoreFeatures features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE};
    const char *extensions[] = {VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions};
    CHECK(vkCreateDevice(c->gpu, &device, NULL, &c->device));
    vkGetDeviceQueue(c->device, family, 0, &c->queue);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    CHECK(vkCreateCommandPool(c->device, &pool, NULL, &c->pool));
}

static VkSemaphore semaphore(struct context *c, int import_fd)
{
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = c->timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY};
    VkExportSemaphoreCreateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .pNext = &type, .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkSemaphoreCreateInfo create = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = import_fd >= 0 && c->import_only ? (void *)&type : (void *)&export};
    VkSemaphore result;
    CHECK(vkCreateSemaphore(c->device, &create, NULL, &result));
    if (import_fd >= 0) {
        PFN_vkImportSemaphoreFdKHR import = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(c->device, "vkImportSemaphoreFdKHR");
        REQUIRE(import);
        VkImportSemaphoreFdInfoKHR info = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = result, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = import_fd};
        CHECK(import(c->device, &info));
    }
    return result;
}

static void buffer(struct context *c, struct allocation *allocation, int import_fd)
{
    VkPhysicalDeviceIDProperties ids = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ids};
    vkGetPhysicalDeviceProperties2(c->gpu, &props);
    if (import_fd >= 0) REQUIRE(!memcmp(ids.deviceUUID, allocation->device_uuid, VK_UUID_SIZE));
    else memcpy(allocation->device_uuid, ids.deviceUUID, VK_UUID_SIZE);
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkBufferCreateInfo create = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = BUFFER_SIZE, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(c->device, &create, NULL, &c->buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(c->device, c->buffer, &requirements);
    if (import_fd < 0) {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(c->gpu, &props);
        uint32_t flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        allocation->memory_type = 0;
        while (allocation->memory_type < props.memoryTypeCount &&
            (!(requirements.memoryTypeBits & (1u << allocation->memory_type)) ||
            (props.memoryTypes[allocation->memory_type].propertyFlags & flags) != flags)) ++allocation->memory_type;
        REQUIRE(allocation->memory_type < props.memoryTypeCount);
        allocation->size = requirements.size;
    }
    REQUIRE(allocation->size == requirements.size && allocation->memory_type < 32 &&
        (requirements.memoryTypeBits & (1u << allocation->memory_type)));
    VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = import_fd};
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = import_fd >= 0 ? (void *)&import : (void *)&export,
        .allocationSize = allocation->size, .memoryTypeIndex = allocation->memory_type};
    CHECK(vkAllocateMemory(c->device, &alloc, NULL, &c->memory));
    CHECK(vkBindBufferMemory(c->device, c->buffer, c->memory, 0));
    CHECK(vkMapMemory(c->device, c->memory, 0, BUFFER_SIZE, 0, (void **)&c->mapped));
}

static void submit(struct context *c, VkSemaphore wait, uint64_t wait_value,
                   VkSemaphore signal, uint64_t signal_value, uint32_t pattern)
{
    VkCommandBuffer command;
    VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(c->device, &alloc, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    if (pattern) vkCmdFillBuffer(command, c->buffer, 0, BUFFER_SIZE, pattern);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command));
    VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = wait ? 1 : 0, .pWaitSemaphoreValues = &wait_value,
        .signalSemaphoreValueCount = signal ? 1 : 0, .pSignalSemaphoreValues = &signal_value};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = c->timeline ? &timeline : NULL,
        .waitSemaphoreCount = wait ? 1 : 0, .pWaitSemaphores = &wait, .pWaitDstStageMask = &stage,
        .commandBufferCount = 1, .pCommandBuffers = &command,
        .signalSemaphoreCount = signal ? 1 : 0, .pSignalSemaphores = &signal};
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(c->device, &fence_info, NULL, &fence));
    CHECK(vkQueueSubmit(c->queue, 1, &submit, fence));
    CHECK(vkWaitForFences(c->device, 1, &fence, VK_TRUE, WAIT_NS));
    vkDestroyFence(c->device, fence, NULL);
    vkFreeCommandBuffers(c->device, c->pool, 1, &command);
}

static void verify(struct context *c, uint32_t value)
{
    for (unsigned i = 0; i < BUFFER_SIZE / sizeof(uint32_t); ++i) REQUIRE(c->mapped[i] == value);
}

static void transfer(int socket, struct allocation *allocation, int fds[3], int sending)
{
    union { struct cmsghdr align; char bytes[CMSG_SPACE(3 * sizeof(int))]; } control = {0};
    struct iovec iov = {.iov_base = allocation, .iov_len = sizeof(*allocation)};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    if (sending) {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(3 * sizeof(int));
        memcpy(CMSG_DATA(cmsg), fds, 3 * sizeof(int));
        REQUIRE(sendmsg(socket, &message, MSG_NOSIGNAL) == sizeof(*allocation));
    } else {
        REQUIRE(recvmsg(socket, &message, MSG_CMSG_CLOEXEC) == sizeof(*allocation));
        REQUIRE(!(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
        REQUIRE(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(3 * sizeof(int)));
        memcpy(fds, CMSG_DATA(cmsg), 3 * sizeof(int));
    }
}

static void finish(struct context *c)
{
    CHECK(vkDeviceWaitIdle(c->device));
    vkUnmapMemory(c->device, c->memory);
    vkDestroyBuffer(c->device, c->buffer, NULL);
    vkFreeMemory(c->device, c->memory, NULL);
    vkDestroySemaphore(c->device, c->forward, NULL);
    if (!c->timeline) vkDestroySemaphore(c->device, c->back, NULL);
    vkDestroyCommandPool(c->device, c->pool, NULL);
    vkDestroyDevice(c->device, NULL);
    vkDestroyInstance(c->instance, NULL);
}

int main(int argc, char **argv)
{
    alarm(30);
    if (argc == 4) {
        struct context c = {.timeline = !strcmp(argv[2], "timeline"),
            .import_only = !strcmp(argv[3], "import-only")};
        struct allocation allocation = {0};
        int fds[3];
        transfer(atoi(argv[1]), &allocation, fds, 0);
        close(atoi(argv[1]));
        init(&c);
        buffer(&c, &allocation, fds[2]);
        c.forward = semaphore(&c, fds[0]);
        if (c.timeline) { c.back = c.forward; close(fds[1]); }
        else c.back = semaphore(&c, fds[1]);
        submit(&c, c.forward, 5, VK_NULL_HANDLE, 0, 0);
        verify(&c, 0x11223344);
        submit(&c, VK_NULL_HANDLE, 0, c.back, 9, 0xa5b6c7d8);
        finish(&c);
        return 0;
    }
    REQUIRE(argc == 1);
    for (unsigned mode_index = 0; mode_index < 4; ++mode_index) {
        unsigned timeline = mode_index & 1, import_only = mode_index >> 1;
        struct context c = {.timeline = timeline, .import_only = import_only};
        struct allocation allocation = {0};
        init(&c);
        buffer(&c, &allocation, -1);
        c.forward = semaphore(&c, -1);
        c.back = timeline ? c.forward : semaphore(&c, -1);
        PFN_vkGetSemaphoreFdKHR get_sem = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(c.device, "vkGetSemaphoreFdKHR");
        PFN_vkGetMemoryFdKHR get_mem = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(c.device, "vkGetMemoryFdKHR");
        REQUIRE(get_sem && get_mem);
        int fds[3];
        VkSemaphoreGetFdInfoKHR sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = c.forward, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
        CHECK(get_sem(c.device, &sem_info, &fds[0]));
        sem_info.semaphore = c.back;
        CHECK(get_sem(c.device, &sem_info, &fds[1]));
        VkMemoryGetFdInfoKHR mem_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = c.memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        CHECK(get_mem(c.device, &mem_info, &fds[2]));
        int sockets[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
        char socket_arg[24];
        snprintf(socket_arg, sizeof(socket_arg), "%d", sockets[1]);
        const char *mode = timeline ? "timeline" : "binary";
        pid_t child = fork();
        REQUIRE(child >= 0);
        if (!child) {
            close(sockets[0]);
            execl("/proc/self/exe", argv[0], socket_arg, mode,
                import_only ? "import-only" : "exportable", (char *)NULL);
            _exit(127);
        }
        close(sockets[1]);
        transfer(sockets[0], &allocation, fds, 1);
        close(sockets[0]);
        for (unsigned i = 0; i < 3; ++i) close(fds[i]);
        submit(&c, VK_NULL_HANDLE, 0, c.forward, 5, 0x11223344);
        submit(&c, c.back, 9, VK_NULL_HANDLE, 0, 0);
        verify(&c, 0xa5b6c7d8);
        int status;
        REQUIRE(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        if (timeline) {
            uint64_t value;
            CHECK(vkGetSemaphoreCounterValue(c.device, c.forward, &value));
            REQUIRE(value == 9);
        }
        finish(&c);
        printf("%s, %s receiver: OPAQUE_FD semaphore + 4 MiB buffer round trip across exec passed\n",
            mode, import_only ? "import-only" : "exportable");
        fflush(stdout);
    }
    return 0;
}
