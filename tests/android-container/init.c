#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void require(int ok, const char *operation)
{
    if (!ok) {
        printf("ANDROID_CONTAINER_FAIL operation=%s errno=%d (%s)\n", operation, errno, strerror(errno));
        _exit(1);
    }
}

static void write_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    require(fd >= 0, path);
    size_t size = strlen(value);
    require(write(fd, value, size) == (ssize_t)size, "write_file");
    require(close(fd) == 0, "close_file");
}

static int binder_device(const char *directory, const char *name, dev_t *device_id)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/binder-control", directory);
    int control = open(path, O_RDONLY | O_CLOEXEC);
    require(control >= 0, "open_binder_control");
    struct binderfs_device device = {0};
    snprintf(device.name, sizeof(device.name), "%s", name);
    require(ioctl(control, BINDER_CTL_ADD, &device) == 0, "allocate_binder_device");
    close(control);
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    require(fd >= 0, "open_binder_device");
    struct binder_version version = {0};
    require(ioctl(fd, BINDER_VERSION, &version) == 0 && version.protocol_version == 8, "binder_protocol_8");
    struct stat status;
    require(fstat(fd, &status) == 0 && S_ISCHR(status.st_mode), "binder_character_device");
    *device_id = status.st_rdev;
    return fd;
}

static void binder_write(int fd, uint32_t command, const void *data, size_t size)
{
    unsigned char buffer[4 + sizeof(struct binder_transaction_data)];
    require(size <= sizeof(buffer) - 4, "binder_command_size");
    memcpy(buffer, &command, 4);
    if (size)
        memcpy(buffer + 4, data, size);
    struct binder_write_read io = {
        .write_size = 4 + size,
        .write_buffer = (uintptr_t)buffer,
    };
    require(ioctl(fd, BINDER_WRITE_READ, &io) == 0 && io.write_consumed == io.write_size,
            "binder_command_written");
}

static struct binder_transaction_data binder_read(int fd, uint32_t expected)
{
    for (int attempt = 0; attempt < 16; ++attempt) {
        unsigned char buffer[1024];
        struct binder_write_read io = {.read_size = sizeof(buffer), .read_buffer = (uintptr_t)buffer};
        require(ioctl(fd, BINDER_WRITE_READ, &io) == 0 && io.read_consumed <= sizeof(buffer),
                "binder_response_read");
        size_t cursor = 0;
        int found = 0;
        struct binder_transaction_data result = {0};
        while (cursor < io.read_consumed) {
            uint32_t command;
            require(io.read_consumed - cursor >= sizeof(command), "binder_response_header");
            memcpy(&command, buffer + cursor, sizeof(command));
            cursor += sizeof(command);
            size_t size = _IOC_SIZE(command);
            require(size <= io.read_consumed - cursor, "binder_response_size");
            if (command == expected) {
                require(!found && size == sizeof(result), "single_binder_response");
                memcpy(&result, buffer + cursor, sizeof(result));
                found = 1;
            } else {
                require(command == BR_NOOP || command == BR_TRANSACTION_COMPLETE,
                        "unexpected_binder_response");
            }
            cursor += size;
        }
        if (found)
            return result;
    }
    require(0, "binder_response_limit");
    return (struct binder_transaction_data){0};
}

static void *binder_mapping(int fd)
{
    void *mapping = mmap(NULL, 128 * 1024, PROT_READ, MAP_PRIVATE, fd, 0);
    require(mapping != MAP_FAILED, "binder_transaction_mapping");
    return mapping;
}

static void binder_buffer(void *mapping, binder_uintptr_t address, size_t size)
{
    uintptr_t base = (uintptr_t)mapping;
    require(address >= base && size <= 128 * 1024 && address - base <= 128 * 1024 - size,
            "binder_mapped_buffer_bounds");
}

static void binder_transaction_test(void)
{
    const char *path = "/work/binder-a/transaction-binder";
    dev_t id;
    int server = binder_device("/work/binder-a", "transaction-binder", &id);
    void *server_map = binder_mapping(server);
    struct flat_binder_object manager = {.flags = FLAT_BINDER_FLAG_ACCEPTS_FDS};
    require(ioctl(server, BINDER_SET_CONTEXT_MGR_EXT, &manager) == 0, "transaction_context_manager");
    pid_t client = fork();
    require(client >= 0, "transaction_client_fork");
    struct payload {
        uint64_t value;
        struct binder_fd_object file;
    };
    if (!client) {
        close(server);
        munmap(server_map, 128 * 1024);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        require(fd >= 0, "transaction_client_open");
        void *mapping = binder_mapping(fd);
        int memory = memfd_create("binder-payload", MFD_CLOEXEC);
        require(memory >= 0 && write(memory, "binder-fd-payload", 17) == 17, "transaction_memfd");
        struct payload payload = {.value = UINT64_C(0x41524d4144415652),
                                  .file = {.hdr = {.type = BINDER_TYPE_FD}, .fd = memory}};
        binder_size_t offset = offsetof(struct payload, file);
        struct binder_transaction_data request = {
            .target.handle = 0, .code = 0x4152, .flags = TF_ACCEPT_FDS,
            .data_size = sizeof(payload), .offsets_size = sizeof(offset),
            .data.ptr = {.buffer = (uintptr_t)&payload, .offsets = (uintptr_t)&offset},
        };
        binder_write(fd, BC_TRANSACTION, &request, sizeof(request));
        struct binder_transaction_data reply = binder_read(fd, BR_REPLY);
        require(reply.data_size == sizeof(uint64_t) && reply.offsets_size == 0 && !(reply.flags & TF_STATUS_CODE),
                "transaction_reply_shape");
        binder_buffer(mapping, reply.data.ptr.buffer, reply.data_size);
        uint64_t value;
        memcpy(&value, (void *)(uintptr_t)reply.data.ptr.buffer, sizeof(value));
        require(value == (payload.value ^ UINT64_C(0xf0f0f0f0f0f0f0f0)), "transaction_reply_value");
        binder_write(fd, BC_FREE_BUFFER, &reply.data.ptr.buffer, sizeof(binder_uintptr_t));
        close(memory);
        munmap(mapping, 128 * 1024);
        close(fd);
        _exit(0);
    }
    binder_write(server, BC_ENTER_LOOPER, NULL, 0);
    struct binder_transaction_data request = binder_read(server, BR_TRANSACTION);
    require(request.code == 0x4152 && request.sender_pid == client && request.sender_euid == getuid()
            && request.data_size == sizeof(struct payload) && request.offsets_size == sizeof(binder_size_t),
            "transaction_sender_and_shape");
    binder_buffer(server_map, request.data.ptr.buffer, request.data_size);
    binder_buffer(server_map, request.data.ptr.offsets, request.offsets_size);
    struct payload received;
    binder_size_t offset;
    memcpy(&received, (void *)(uintptr_t)request.data.ptr.buffer, sizeof(received));
    memcpy(&offset, (void *)(uintptr_t)request.data.ptr.offsets, sizeof(offset));
    require(received.value == UINT64_C(0x41524d4144415652) && offset == offsetof(struct payload, file)
            && received.file.hdr.type == BINDER_TYPE_FD, "transaction_received_payload");
    char contents[17];
    require(pread(received.file.fd, contents, sizeof(contents), 0) == sizeof(contents)
            && !memcmp(contents, "binder-fd-payload", sizeof(contents)), "transaction_received_fd");
    close(received.file.fd);
    uint64_t value = received.value ^ UINT64_C(0xf0f0f0f0f0f0f0f0);
    struct binder_transaction_data reply = {.data_size = sizeof(value), .data.ptr.buffer = (uintptr_t)&value};
    binder_write(server, BC_REPLY, &reply, sizeof(reply));
    binder_write(server, BC_FREE_BUFFER, &request.data.ptr.buffer, sizeof(binder_uintptr_t));
    int status;
    require(waitpid(client, &status, 0) == client && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "transaction_client_passed");
    munmap(server_map, 128 * 1024);
    close(server);
    require(unlink(path) == 0, "transaction_device_removed");
    puts("ANDROID_BINDER_TRANSACTION_PASS request=1 reply=1 fd=1 sender_identity=1");
}

static void binder_pollfree_test(void)
{
    dev_t id;
    int device = binder_device("/work/binder-a", "poll-binder", &id);
    close(device);
    for (int round = 0; round < 32; ++round) {
        int fd = open("/work/binder-a/poll-binder", O_RDWR | O_CLOEXEC);
        int epoll = epoll_create1(EPOLL_CLOEXEC);
        require(fd >= 0 && epoll >= 0, "pollfree_open");
        struct epoll_event event = {.events = EPOLLIN, .data.fd = fd};
        require(epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &event) == 0, "pollfree_register");
        require(epoll_wait(epoll, &event, 1, 0) >= 0, "pollfree_initial_wait");
        int zero = 0;
        require(ioctl(fd, BINDER_THREAD_EXIT, &zero) == 0, "pollfree_thread_exit");
        require(epoll_wait(epoll, &event, 1, 0) >= 0, "pollfree_wait_after_exit");
        close(fd);
        close(epoll);
    }
    require(unlink("/work/binder-a/poll-binder") == 0, "pollfree_device_removed");
    puts("ANDROID_BINDER_POLLFREE_PASS iterations=32");
}

static void binder_test(void)
{
    require(mkdir("/work/binder-a", 0700) == 0 && mkdir("/work/binder-b", 0700) == 0, "binder_directories");
    require(mount("binder", "/work/binder-a", "binder", 0, "max=6") == 0, "mount_binder_a");
    require(mount("binder", "/work/binder-b", "binder", 0, "max=6") == 0, "mount_binder_b");
    const char *names[] = {"anbox-binder", "anbox-hwbinder", "anbox-vndbinder"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        dev_t first, second;
        int a = binder_device("/work/binder-a", names[i], &first);
        int b = binder_device("/work/binder-b", names[i], &second);
        require(first != second, "separate_binder_devices");
        int zero = 0;
        require(ioctl(a, BINDER_SET_CONTEXT_MGR, &zero) == 0, "first_context_manager");
        require(ioctl(b, BINDER_SET_CONTEXT_MGR, &zero) == 0, "independent_context_manager");
        char path[256];
        snprintf(path, sizeof(path), "/work/binder-a/%s", names[i]);
        int duplicate = open(path, O_RDWR | O_CLOEXEC);
        require(duplicate >= 0, "open_duplicate_binder");
        errno = 0;
        require(ioctl(duplicate, BINDER_SET_CONTEXT_MGR, &zero) == -1 && errno == EBUSY,
                "same_context_rejects_second_manager");
        close(duplicate);
        require(unlink(path) == 0, "remove_binder_a");
        struct binder_version version = {0};
        require(ioctl(b, BINDER_VERSION, &version) == 0 && version.protocol_version == 8,
                "binder_b_survives_removal");
        close(a);
        close(b);
        printf("ANDROID_BINDER_PASS name=%s protocol=8 independent_contexts=2\n", names[i]);
    }
    binder_transaction_test();
    binder_pollfree_test();
    errno = 0;
    require(unlink("/work/binder-a/binder-control") == -1 && errno == EPERM, "binder_control_protected");
    require(umount("/work/binder-a") == 0 && umount("/work/binder-b") == 0, "unmount_binder");
    puts("ANDROID_BINDER_ISOLATION_PASS");
}

static void memory_test(void)
{
    int fd = memfd_create("android-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    require(fd >= 0 && ftruncate(fd, 4096) == 0, "memfd_create");
    char *data = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    require(data != MAP_FAILED, "memfd_map");
    memcpy(data, "parent", 7);
    pid_t pid = fork();
    require(pid >= 0, "memory_fork");
    if (!pid) {
        require(!strcmp(data, "parent"), "shared_memory_read");
        memcpy(data, "child", 6);
        _exit(0);
    }
    int status;
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "memory_child");
    require(!strcmp(data, "child") && munmap(data, 4096) == 0, "shared_memory_write");
    require(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == 0,
            "memfd_seals");
    errno = 0;
    require(write(fd, "x", 1) == -1 && errno == EPERM, "sealed_write_denied");
    close(fd);
    puts("ANDROID_MEMFD_PASS");
}

static void overlay_test(void)
{
    const char *paths[] = {"/work/lower", "/work/upper", "/work/overlay-work", "/work/merged"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
        require(mkdir(paths[i], 0700) == 0, "overlay_directory");
    write_file("/work/lower/value", "base");
    int result = mount("overlay", "/work/merged", "overlay", 0,
                       "lowerdir=/work/lower,upperdir=/work/upper,workdir=/work/overlay-work,userxattr");
    if (result) {
        int reason = errno;
        require(reason == EPERM || reason == EINVAL || reason == ENODEV || reason == EOPNOTSUPP,
                "unexpected_overlay_failure");
        printf("ANDROID_ROOTLESS_OVERLAY_UNAVAILABLE errno=%d\n", reason);
        return;
    }
    write_file("/work/merged/value", "changed");
    char data[5] = {0};
    int fd = open("/work/lower/value", O_RDONLY | O_CLOEXEC);
    require(fd >= 0 && read(fd, data, 4) == 4 && !strcmp(data, "base"), "overlay_base_unchanged");
    close(fd);
    require(umount("/work/merged") == 0, "unmount_overlay");
    puts("ANDROID_ROOTLESS_OVERLAY_PASS");
}

static void seccomp_test(void)
{
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {sizeof(filter) / sizeof(filter[0]), filter};
    require(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0, "no_new_privileges");
    require(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0, "seccomp_filter");
    errno = 0;
    require(syscall(SYS_getppid) == -1 && errno == EPERM, "seccomp_denied_syscall");
    require(syscall(SYS_getpid) > 0, "seccomp_allowed_syscall");
    puts("ANDROID_SECCOMP_PASS");
}

static void child_test(void)
{
    require(setgroups(0, NULL) == 0 && setgid(1000) == 0 && setuid(1000) == 0, "drop_host_privileges");
    require(getuid() == 1000 && geteuid() == 1000, "unprivileged_host_identity");
    /* Dropping uid clears dumpability and would leave our proc id-map files root-owned. */
    require(prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == 0, "user_owned_proc_files");
    require(unshare(CLONE_NEWUSER) == 0, "unprivileged_user_namespace");
    write_file("/proc/self/setgroups", "deny");
    write_file("/proc/self/uid_map", "0 1000 1\n");
    write_file("/proc/self/gid_map", "0 1000 1\n");
    require(getuid() == 0 && getgid() == 0, "container_root_identity");
    require(unshare(CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID) == 0,
            "container_namespaces");
    require(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0, "private_mounts");
    pid_t container = fork();
    require(container >= 0, "pid_namespace_fork");
    if (container) {
        int status;
        require(waitpid(container, &status, 0) == container && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "container_init_exit");
        return;
    }
    require(getpid() == 1 && getppid() == 0, "container_pid_namespace");
    require(mount("tmpfs", "/work", "tmpfs", 0, "size=8m,mode=0700") == 0, "container_tmpfs");
    require(mkdir("/work/proc", 0700) == 0, "container_proc_directory");
    require(mount("proc", "/work/proc", "proc", 0, NULL) == 0, "container_proc_mount");
    puts("ANDROID_PIDNS_PASS container_pid=1");
    puts("ANDROID_USERNS_PASS host_uid=1000 container_uid=0");
    binder_test();
    memory_test();
    overlay_test();
    seccomp_test();
    puts("ANDROID_CONTAINER_CHILD_PASS");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getpid() != 1)
        return 2;
    mkdir("/proc", 0755);
    require(mount("proc", "/proc", "proc", 0, NULL) == 0, "mount_proc");
    mkdir("/sys", 0755);
    require(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0, "mount_sys");
    char compatible[128] = {0}, command[2048] = {0};
    FILE *stream = fopen("/proc/device-tree/compatible", "r");
    if (!stream || !fgets(compatible, sizeof(compatible), stream) || strcmp(compatible, "linux,dummy-virt"))
        return 2;
    fclose(stream);
    stream = fopen("/proc/cmdline", "r");
    if (!stream || !fgets(command, sizeof(command), stream) || !strstr(command, "armada.android_container_test=1"))
        return 2;
    fclose(stream);
    mkdir("/dev", 0755);
    require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0, "mount_dev");
    require(mkdir("/work", 0777) == 0 && chmod("/work", 0777) == 0, "work_directory");
    pid_t pid = fork();
    require(pid >= 0, "test_fork");
    if (!pid) {
        child_test();
        _exit(0);
    }
    int status = 0;
    int passed = waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    struct stat mount_status;
    passed = passed && stat("/work/binder-a", &mount_status) == -1 && errno == ENOENT;
    puts(passed ? "ANDROID_CONTAINER_KERNEL_PASS" : "ANDROID_CONTAINER_KERNEL_FAIL");
    sync();
    reboot(RB_POWER_OFF);
    return 1;
}
