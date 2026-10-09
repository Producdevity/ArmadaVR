import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("android_container", Path(__file__).parents[1] / "tools/test-android-container.py")
container = importlib.util.module_from_spec(spec)
spec.loader.exec_module(container)


class AndroidContainerEvidenceTests(unittest.TestCase):
    def log(self):
        return "\n".join([
            "ANDROID_PIDNS_PASS container_pid=1",
            "ANDROID_USERNS_PASS host_uid=1000 container_uid=0",
            *[f"ANDROID_BINDER_PASS name={name} protocol=8 independent_contexts=2"
              for name in ("anbox-binder", "anbox-hwbinder", "anbox-vndbinder")],
            "ANDROID_BINDER_ISOLATION_PASS",
            "ANDROID_BINDER_TRANSACTION_PASS request=1 reply=1 fd=1 sender_identity=1",
            "ANDROID_BINDER_POLLFREE_PASS iterations=32",
            "ANDROID_MEMFD_PASS", "ANDROID_SECCOMP_PASS", "ANDROID_CONTAINER_CHILD_PASS",
            "ANDROID_CONTAINER_KERNEL_PASS", "ANDROID_ROOTLESS_OVERLAY_UNAVAILABLE errno=1",
            "reboot: Power down",
        ])

    def test_incomplete_or_duplicated_transaction_evidence_is_rejected(self):
        for marker in ("ANDROID_BINDER_TRANSACTION_PASS request=1 reply=1 fd=1 sender_identity=1",
                       "ANDROID_BINDER_POLLFREE_PASS iterations=32"):
            for log in (self.log().replace(marker, ""), self.log() + "\n" + marker):
                with self.subTest(marker=marker, log=log), self.assertRaises(RuntimeError):
                    container.check_log(log)

    def test_guest_failure_overrides_success_markers(self):
        for failure in ("WARNING:", "BUG:", "Oops:", "Kernel panic", "ANDROID_CONTAINER_FAIL"):
            with self.subTest(failure=failure), self.assertRaises(RuntimeError):
                container.check_log(self.log() + "\n" + failure)

    def test_overlay_failure_does_not_become_runtime_success(self):
        result = container.check_log(self.log())
        self.assertTrue(result["binder_transaction_and_fd_passing"])
        self.assertEqual(result["binder_pollfree_iterations"], 32)
        self.assertFalse(result["native_rootless_overlayfs"])
