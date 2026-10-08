#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CHECK(call)                                                                                          \
    do {                                                                                                     \
        VkResult r = (call);                                                                                 \
        if (r) {                                                                                             \
            fprintf(stderr, "%s: %d\n", #call, r);                                                           \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
#define REQUIRE(test)                                                                                        \
    do {                                                                                                     \
        if (!(test)) {                                                                                       \
            fprintf(stderr, "%s failed\n", #test);                                                           \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
#define SIZE (64 * 64 * 4)
struct context {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkCommandPool pool;
    VkImage image;
    VkDeviceMemory memory;
};

static void initialize(struct context *c)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                     .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&instance, NULL, &c->instance));
    uint32_t n = 1;
    CHECK(vkEnumeratePhysicalDevices(c->instance, &n, &c->gpu));
    REQUIRE(n == 1);
    uint32_t capacity = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->gpu, &capacity, NULL);
    REQUIRE(capacity && capacity <= 4096);
    VkQueueFamilyProperties *families = calloc(capacity, sizeof(*families));
    REQUIRE(families);
    n = capacity;
    vkGetPhysicalDeviceQueueFamilyProperties(c->gpu, &n, families);
    REQUIRE(n <= capacity);
    while (c->family < n && !(families[c->family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        c->family++;
    REQUIRE(c->family < n);
    free(families);
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = c->family,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    const char *exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &queue,
                                 .enabledExtensionCount = 1,
                                 .ppEnabledExtensionNames = exts};
    CHECK(vkCreateDevice(c->gpu, &device, NULL, &c->device));
    vkGetDeviceQueue(c->device, c->family, 0, &c->queue);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .queueFamilyIndex = c->family};
    CHECK(vkCreateCommandPool(c->device, &pool, NULL, &c->pool));
}

static uint32_t memory_type(struct context *c, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(c->gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    REQUIRE(0);
    return 0;
}

static void image(struct context *c, VkFormat format, VkImageTiling tiling,
                  VkExternalMemoryHandleTypeFlagBits handle, int fd, uint32_t *type, VkImageUsageFlags usage,
                  uint64_t *allocation_size)
{
    VkExternalMemoryImageCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                                .handleTypes = handle};
    VkImageCreateInfo create = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .pNext = &external,
                                .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = format,
                                .extent = {64, 64, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = tiling,
                                .usage = usage,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(c->device, &create, NULL, &c->image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(c->device, c->image, &requirements);
    if (fd < 0)
        *allocation_size = requirements.size;
    else
        REQUIRE(*allocation_size == requirements.size);
    if (fd < 0)
        *type = memory_type(c, requirements.memoryTypeBits, 0);
    REQUIRE(requirements.memoryTypeBits & (1u << *type));
    VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                               .image = c->image};
    VkExportMemoryAllocateInfo export = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &dedicated, .handleTypes = handle};
    VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                      .pNext = &dedicated,
                                      .handleType = handle,
                                      .fd = fd};
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .pNext = fd < 0 ? (void *)&export : (void *)&import,
                                  .allocationSize = requirements.size,
                                  .memoryTypeIndex = *type};
    CHECK(vkAllocateMemory(c->device, &alloc, NULL, &c->memory));
    CHECK(vkBindImageMemory(c->device, c->image, c->memory, 0));
}

static void submit(struct context *c, VkBuffer readback, int red)
{
    VkCommandBuffer command;
    VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = c->pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(c->device, &alloc, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = c->image,
        .oldLayout = readback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = readback ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = readback ? c->family : VK_QUEUE_FAMILY_IGNORED,
        .dstAccessMask = readback ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         NULL, 0, NULL, 1, &barrier);
    if (readback) {
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {64, 64, 1}};
        vkCmdCopyImageToBuffer(command, c->image, VK_IMAGE_LAYOUT_GENERAL, readback, 1, &copy);
        VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host,
                             0, NULL, 0, NULL);
    } else {
        VkClearColorValue color = {.float32 = {red ? 1 : 0, 0, red ? 0 : 1, 1}};
        vkCmdClearColorImage(command, c->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1,
                             &barrier.subresourceRange);
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.srcQueueFamilyIndex = c->family;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, NULL, 0, NULL, 1, &barrier);
    }
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(c->device, &info, NULL, &fence));
    CHECK(vkQueueSubmit(c->queue, 1, &submit, fence));
    CHECK(vkWaitForFences(c->device, 1, &fence, VK_TRUE, 2000000000ULL));
    vkDestroyFence(c->device, fence, NULL);
}

static void finish(struct context *c)
{
    vkDestroyImage(c->device, c->image, NULL);
    vkFreeMemory(c->device, c->memory, NULL);
    vkDestroyCommandPool(c->device, c->pool, NULL);
    vkDestroyDevice(c->device, NULL);
    vkDestroyInstance(c->instance, NULL);
}

static void device_uuid(struct context *c, char out[33])
{
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                         .pNext = &id};
    vkGetPhysicalDeviceProperties2(c->gpu, &props);
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i)
        snprintf(out + i * 2, 3, "%02x", id.deviceUUID[i]);
}

static int fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    REQUIRE(directory);
    int count = -1;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.')
            ++count;
    REQUIRE(closedir(directory) == 0);
    return count;
}

static void read_pixels(int fd, VkFormat format, VkImageTiling tiling, uint32_t type, VkImageUsageFlags usage,
                        uint64_t size, const char *uuid, int red)
{
    struct context c = {0};
    initialize(&c);
    char actual[33];
    device_uuid(&c, actual);
    REQUIRE(!strcmp(actual, uuid));
    image(&c, format, tiling, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, fd, &type, usage, &size);
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = SIZE,
                                      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer;
    CHECK(vkCreateBuffer(c.device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(c.device, buffer, &requirements);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = requirements.size,
                                  .memoryTypeIndex = memory_type(&c, requirements.memoryTypeBits,
                                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(c.device, &alloc, NULL, &memory));
    CHECK(vkBindBufferMemory(c.device, buffer, memory, 0));
    submit(&c, buffer, red);
    uint8_t *pixels;
    CHECK(vkMapMemory(c.device, memory, 0, SIZE, 0, (void **)&pixels));
    int bgra = format == VK_FORMAT_B8G8R8A8_UNORM;
    uint8_t expected[4] = {red ? (bgra ? 0 : 255) : (bgra ? 255 : 0), 0,
                           red ? (bgra ? 255 : 0) : (bgra ? 0 : 255), 255};
    for (unsigned i = 0; i < SIZE; i += 4)
        REQUIRE(memcmp(pixels + i, expected, 4) == 0);
    vkUnmapMemory(c.device, memory);
    vkDestroyBuffer(c.device, buffer, NULL);
    vkFreeMemory(c.device, memory, NULL);
    finish(&c);
    printf("IMAGE_CHILD_PASS pid=%u format=%d tiling=%d usage=%x color=%s pixels=4096 uuid=%s\n",
           (unsigned)getpid(), format, tiling, usage, red ? "red" : "blue", actual);
}

static uint64_t number(const char *value, uint64_t maximum)
{
    REQUIRE(value[0] >= '0' && value[0] <= '9');
    errno = 0;
    char *end;
    unsigned long long result = strtoull(value, &end, 10);
    REQUIRE(errno == 0 && *end == '\0' && result <= maximum);
    return result;
}

int main(int argc, char **argv)
{
    alarm(45);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 10 && !strcmp(argv[1], "--read")) {
        REQUIRE(strlen(argv[8]) == 32);
        read_pixels(number(argv[2], INT_MAX), number(argv[3], UINT32_MAX), number(argv[4], UINT32_MAX),
                    number(argv[5], 31), number(argv[6], UINT32_MAX), number(argv[7], UINT64_MAX), argv[8],
                    number(argv[9], 1));
        return 0;
    }
    REQUIRE(argc == 1);
    unsigned passed = 0;
    int initial_fds = fd_count();
    for (unsigned bgra = 0; bgra < 2; ++bgra)
        for (unsigned linear = 0; linear < 2; ++linear)
            for (unsigned compute = 0; compute < 2; ++compute)
                for (unsigned red = 0; red < 2; ++red) {
                    struct context c = {0};
                    initialize(&c);
                    VkFormat format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
                    VkImageTiling tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
                    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                              VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                    if (compute)
                        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
                    VkPhysicalDeviceExternalImageFormatInfo external = {
                        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
                    VkPhysicalDeviceImageFormatInfo2 query = {
                        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                        .pNext = &external,
                        .format = format,
                        .type = VK_IMAGE_TYPE_2D,
                        .tiling = tiling,
                        .usage = usage,
                        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT};
                    VkExternalImageFormatProperties extprops = {
                        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
                    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                                                      .pNext = &extprops};
                    CHECK(vkGetPhysicalDeviceImageFormatProperties2(c.gpu, &query, &props));
                    VkExternalMemoryFeatureFlags features =
                        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
                    REQUIRE((extprops.externalMemoryProperties.externalMemoryFeatures & features) ==
                            features);
                    uint32_t type = 0;
                    uint64_t size = 0;
                    image(&c, format, tiling, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, -1, &type, usage,
                          &size);
                    submit(&c, VK_NULL_HANDLE, red);
                    PFN_vkGetMemoryFdKHR get =
                        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(c.device, "vkGetMemoryFdKHR");
                    VkMemoryGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                                    .memory = c.memory,
                                                    .handleType =
                                                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
                    int fd;
                    REQUIRE(get);
                    CHECK(get(c.device, &fd_info, &fd));
                    REQUIRE(fcntl(fd, F_SETFD, 0) == 0);
                    char values[7][32], uuid[33];
                    device_uuid(&c, uuid);
                    snprintf(values[0], 32, "%d", fd);
                    snprintf(values[1], 32, "%u", format);
                    snprintf(values[2], 32, "%u", tiling);
                    snprintf(values[3], 32, "%u", type);
                    snprintf(values[4], 32, "%u", usage);
                    snprintf(values[5], 32, "%llu", (unsigned long long)size);
                    snprintf(values[6], 32, "%u", red);
                    pid_t pid = fork();
                    REQUIRE(pid >= 0);
                    if (pid == 0) {
                        execl("/proc/self/exe", argv[0], "--read", values[0], values[1], values[2], values[3],
                              values[4], values[5], uuid, values[6], NULL);
                        _exit(127);
                    }
                    int status = 0;
                    pid_t waited;
                    do {
                        waited = waitpid(pid, &status, 0);
                    } while (waited < 0 && errno == EINTR);
                    REQUIRE(waited == pid);
                    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
                    close(fd);
                    finish(&c);
                    REQUIRE(fd_count() == initial_fds);
                    printf("IMAGE_PARENT_PASS child=%u format=%u linear=%u compute=%u color=%s fds=%d\n",
                           (unsigned)pid, format, linear, compute, red ? "red" : "blue", fd_count());
                    ++passed;
                }
    printf("EXTERNAL_IMAGE_PASS cases=%u pixels=%u initial_fds=%d final_fds=%d compositor_verified=false "
           "hardware_verified=false\n",
           passed, passed * 4096, initial_fds, fd_count());
    return 0;
}
