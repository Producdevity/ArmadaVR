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
