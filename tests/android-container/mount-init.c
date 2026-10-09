#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/reboot.h>

static int check_board(void)
{
    char board[128] = {0}, cmdline[2048] = {0};
    FILE *file = fopen("/proc/device-tree/compatible", "r");
    if (!file || !fgets(board, sizeof(board), file) || strcmp(board, "linux,dummy-virt"))
        return 0;
    fclose(file);
    file = fopen("/proc/cmdline", "r");
    if (!file || !fgets(cmdline, sizeof(cmdline), file))
        return 0;
    fclose(file);
    for (char *token = strtok(cmdline, " \n"); token; token = strtok(NULL, " \n")) {
        if (!strcmp(token, "armada.lepton_mount_test=1"))
            return 1;
    }
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getpid() != 1)
        return 2;
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    if (mount("proc", "/proc", "proc", 0, NULL) || mount("sysfs", "/sys", "sysfs", 0, NULL) || !check_board())
        return 2;
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL))
        return 2;
    if (access("/mount-root.sh", F_OK)) {
        mkdir("/newroot", 0755);
        if (mount("/dev/vda", "/newroot", "ext4", 0, NULL)) {
            perror("guest_root_mount");
            return 2;
        }
        umount("/sys");
        umount("/proc");
        umount("/dev");
        if (chdir("/newroot") || mount(".", "/", NULL, MS_MOVE, NULL) || chroot(".") || chdir("/")) {
            perror("switch_root");
            return 2;
        }
        execl("/init", "init", NULL);
        perror("root_init");
        return 2;
    }
    pid_t pid = fork();
    if (!pid) {
        execl("/bin/sh", "sh", "/mount-root.sh", NULL);
        _exit(127);
    }
    int status = 0, fixture_status = 0, fixture_done = 0, children_gone = 0;
    while (pid > 0 && !fixture_done) {
        pid_t child = waitpid(-1, &status, 0);
        if (child < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (child == pid) {
            fixture_status = status;
            fixture_done = 1;
        }
    }
    for (int attempt = 0; fixture_done && attempt < 200; ++attempt) {
        pid_t child = waitpid(-1, &status, WNOHANG);
        if (child < 0 && errno == ECHILD) {
            children_gone = 1;
            puts("LEPTON_MOUNT_CHILDREN_REAPED_PASS");
            break;
        }
        if (child > 0)
            continue;
        usleep(10000);
    }
    int passed = fixture_done && children_gone && WIFEXITED(fixture_status) && WEXITSTATUS(fixture_status) == 0;
    puts(passed ? "LEPTON_MOUNT_VM_PASS" : "LEPTON_MOUNT_VM_FAIL");
    sync();
    reboot(RB_POWER_OFF);
    return 1;
}
