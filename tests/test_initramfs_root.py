import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("root_test", Path(__file__).parents[1] / "tools/test-initramfs-root.py")
root_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(root_test)


class RootMachineTests(unittest.TestCase):
    def test_pico_requires_secure_monitor_and_reservation_space(self):
        options = root_test.machine_options({"repository": "bytedance/neo3-kernel"}, None)
        self.assertEqual(options[options.index("-machine") + 1], "virt,secure=on,virtualization=on")
        self.assertEqual(options[options.index("-accel") + 1], "tcg")
        self.assertEqual(options[options.index("-m") + 1], "2048")

    def test_pico_rejects_accelerators_without_secure_monitor(self):
        for accel in ("hvf", "kvm"):
            with self.subTest(accel=accel), self.assertRaisesRegex(ValueError, "requires TCG"):
                root_test.machine_options({"repository": "bytedance/neo3-kernel"}, accel)

    def test_quest_keeps_host_cpu_with_hardware_acceleration(self):
        options = root_test.machine_options({"repository": "facebookincubator/oculus-linux-kernel"}, "hvf")
        self.assertEqual(options[options.index("-machine") + 1], "virt")
        self.assertEqual(options[options.index("-cpu") + 1], "host")


class RootServiceTests(unittest.TestCase):
    def test_missing_root_reports_either_dracut_or_device_timeout(self):
        for log in ("Warning: /dev/disk/by-label/absent-root does not exist",
                    "Timed out waiting for device \x1b[0;1;39m/dev/disk/by-label/absent-root\x1b[0m."):
            with self.subTest(log=log):
                self.assertTrue(root_test.missing_root_failure(log))
        for log in ("root=LABEL=absent-root", "Expecting device /dev/disk/by-label/absent-root...",
                    "Timed out waiting for device /dev/disk/by-label/other-root."):
            with self.subTest(log=log):
                self.assertFalse(root_test.missing_root_failure(log))

    def test_handoff_marker_does_not_hide_failed_credentials(self):
        log = """systemd[1]: systemd-journald.service: Failed with result 'exit-code'.
(systemd-journald)[278]: systemd-journald.service: Failed at step CREDENTIALS spawning /usr/lib/systemd/systemd-journald: Function not implemented
systemd[1]: systemd-tmpfiles-setup.service: Failed with result 'exit-code'.
ROOT_BLOCK_WRITES=0
ARMADA_ROOT_OVERLAY_PASS
reboot: Power down
"""
        self.assertEqual(root_test.service_failures(log),
                         ["systemd-journald.service", "systemd-tmpfiles-setup.service"])

    def test_optional_kernel_warning_is_not_a_service_failure(self):
        log = """systemd[1]: Failed to load optional BPF support: Operation not supported
systemd[1]: Started systemd-journald.service - Journal Service.
ARMADA_ROOT_OVERLAY_PASS
reboot: Power down
"""
        self.assertEqual(root_test.service_failures(log), [])

    def test_console_failure_is_rejected_after_journal_takes_over(self):
        log = "[\x1b[0;1;31mFAILED\x1b[0m] Failed to start \x1b[0;1;39mApply Kernel Variables\x1b[0m.\r\n"
        self.assertEqual(root_test.service_failures(log), ["Failed to start Apply Kernel Variables."])
