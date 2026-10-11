from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class LauncherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if compiler is None:
            raise unittest.SkipTest('A host C compiler is required')
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        source = Path(__file__).parents[1] / 'system/systemd/launcher.c'
        harness = r'''
#define main launcher_main
#define getpid test_getpid
#define readlink test_readlink
#define execv test_execv
#include "SOURCE"
#undef main
static const char *scenario;
pid_t test_getpid(void) { return !strcmp(scenario, "child-no-proc") ? 2 : 1; }
ssize_t test_readlink(const char *name, char *buffer, size_t size) {
    (void)name;
    if (strcmp(scenario, "shutdown-proc")) { errno = ENOENT; return -1; }
    const char *path = "/usr/lib/systemd/systemd-shutdown";
    if (strlen(path) >= size) return -1;
    memcpy(buffer, path, strlen(path));
    return (ssize_t)strlen(path);
}
int test_execv(const char *file, char *const args[]) {
    puts(file);
    for (int i = 0; args[i]; i++) puts(args[i]);
    errno = ENOEXEC;
    return -1;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    scenario = argv[1];
    return launcher_main(argc - 2, argv + 2);
}
'''.replace('SOURCE', str(source))
        (root / 'test.c').write_text(harness)
        cls.binary = root / 'test'
        cls.native_binary = root / 'test-native'
        subprocess.run([compiler, '-Wall', '-Wextra', '-Werror', str(root / 'test.c'),
                        '-o', str(cls.binary)], capture_output=True, text=True, check=True, timeout=30)
        subprocess.run([compiler, '-Wall', '-Wextra', '-Werror', '-DARMADA_SYSTEMD_NATIVE_LIBRARIES=1',
                        str(root / 'test.c'), '-o', str(cls.native_binary)],
                       capture_output=True, text=True, check=True, timeout=30)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def invoke(self, scenario, argv0):
        result = subprocess.run([str(self.binary), scenario, argv0, 'poweroff'],
                                capture_output=True, text=True, check=False, timeout=5)
        self.assertEqual(result.returncode, 126)
        return result.stdout.splitlines()

    def test_pid_one_startup_before_proc_mount(self):
        lines = self.invoke('init-no-proc', '/init')
        self.assertEqual(lines[-2:], ['/opt/armada-vr/systemd/usr/lib/systemd/systemd', 'poweroff'])
        self.assertIn('/init', lines)

    def test_pid_one_selects_shutdown_helper_after_proc_is_mounted(self):
        lines = self.invoke('shutdown-proc', '/usr/lib/systemd/systemd-shutdown')
        self.assertEqual(lines[-2:], ['/opt/armada-vr/systemd/usr/lib/systemd/systemd-shutdown', 'poweroff'])

    def test_pid_one_shutdown_without_proc(self):
        lines = self.invoke('shutdown-no-proc', '/usr/lib/systemd/systemd-shutdown')
        self.assertEqual(lines[-2:], ['/opt/armada-vr/systemd/usr/lib/systemd/systemd-shutdown', 'poweroff'])

    def test_unknown_pid_one_and_unresolved_child_are_refused(self):
        self.assertEqual(self.invoke('init-no-proc', '/unexpected'), [])
        self.assertEqual(self.invoke('child-no-proc', '/init'), [])

    def test_native_root_keeps_host_loader_and_library_precedence(self):
        result = subprocess.run([str(self.native_binary), 'init-no-proc', '/sbin/init'],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 126)
        lines = result.stdout.splitlines()
        self.assertEqual(lines[0], '/lib/ld-linux-aarch64.so.1')
        self.assertIn('/usr/lib64:/opt/armada-vr/systemd/usr/lib:/opt/armada-vr/systemd/usr/lib/systemd', lines)
        self.assertEqual(lines[-1], '/opt/armada-vr/systemd/usr/lib/systemd/systemd')
