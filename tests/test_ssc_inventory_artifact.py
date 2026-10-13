import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('inventory', Path(__file__).parents[1] / 'tools/ssc-inventory-artifact.py')
artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifact)


class InventoryArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.build, self.bundle, self.root = [self.path / n for n in ('build', 'bundle', 'root')]
        self.build.mkdir()
        self.root.mkdir()
        dep_prefix = self.root / artifact.ssc.INSTALL_PREFIX
        dep_prefix.mkdir(parents=True)
        (dep_prefix / 'library').write_bytes(b'SSC dependency fixture')
        dep_record = self.root / artifact.ssc.BUILD_RECORD
        dep_record.parent.mkdir(parents=True)
        dep_record.write_text('dependency record')
        self.dependency = {'contents': artifact.ssc.contents(dep_prefix),
                           'build_sha256': artifact.ssc.digest(dep_record)}
        binary = self.build / 'ssc-inventory-release'
        binary.write_bytes(b'\x7fELF\x02\x01\x01' + bytes(11) + b'\xb7\x00' + b'fixture')
        self.result = {'status': 'passed', 'hardware_verified': False, 'ssc_bundle': self.dependency,
                       'inputs_sha256': {n: artifact.inputs()[n] for n in artifact.INPUTS},
                       'binaries_sha256': {binary.name: artifact.ssc.digest(binary)}}
        (self.build / 'result.json').write_text(json.dumps(self.result))

    def package(self):
        with patch.object(artifact.ssc, 'package', return_value=self.dependency):
            return artifact.build(self.build, self.path / 'ssc', self.bundle)

    def test_install_readback_and_no_startup(self):
        record = self.package()
        artifact.install(self.bundle, self.root, record)
        artifact.verify_install(self.root, record)
        self.assertFalse(record['automatic_startup'])
        self.assertEqual((self.root / artifact.PREFIX / 'bin/ssc-inventory').read_text(), artifact.LAUNCHER)
        self.assertFalse((self.root / 'usr/lib/systemd').exists())
        with self.assertRaisesRegex(ValueError, 'already supplies'):
            artifact.install(self.bundle, self.root, record)

    def test_modified_build_and_dependency_are_rejected(self):
        for key, value in [('status', 'failed'), ('hardware_verified', True),
                           ('ssc_bundle', {}), ('inputs_sha256', {}), ('binaries_sha256', {})]:
            with self.subTest(key=key):
                (self.build / 'result.json').write_text(json.dumps(dict(self.result, **{key: value})))
                with self.assertRaisesRegex(ValueError, 'unchanged verified'):
                    self.package()
                self.assertFalse(self.bundle.exists())

    def test_installer_requires_exact_installed_dependency_before_copying(self):
        record = self.package()
        (self.root / artifact.ssc.BUILD_RECORD).write_text('different dependency')
        with self.assertRaisesRegex(ValueError, 'SSC build record'):
            artifact.install(self.bundle, self.root, record)
        self.assertFalse((self.root / artifact.PREFIX).exists())

    def test_modified_binary_and_source_are_rejected(self):
        record = self.package()
        source = self.bundle / artifact.PREFIX / 'share/source/tools/ssc-inventory.c'
        original = source.read_bytes()
        source.write_bytes(original + b'changed')
        with self.assertRaisesRegex(ValueError, 'contents'):
            artifact.package(self.bundle, self.dependency)
        with self.assertRaisesRegex(ValueError, 'changed before installation'):
            artifact.install(self.bundle, self.root, record)
        self.assertFalse((self.root / artifact.PREFIX).exists())

    def test_destination_and_input_links_are_rejected(self):
        record = self.package()
        prefix = self.root / artifact.PREFIX
        prefix.symlink_to(self.path / 'missing')
        with self.assertRaisesRegex(ValueError, 'symlink'):
            artifact.install(self.bundle, self.root, record)
        prefix.unlink()
        binary = self.bundle / artifact.PREFIX / 'libexec/ssc-inventory'
        binary.unlink()
        binary.symlink_to('/etc/passwd')
        with self.assertRaises(ValueError):
            artifact.install(self.bundle, self.root, record)

    def test_foreign_elf_and_startup_claim_are_rejected(self):
        self.package()
        metadata = self.bundle / 'build.json'
        original = metadata.read_text()
        value = json.loads(original)
        value['automatic_startup'] = True
        metadata.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, 'package or SSC dependency'):
            artifact.package(self.bundle, self.dependency)
        metadata.write_text(original)
        binary = self.bundle / artifact.PREFIX / 'libexec/ssc-inventory'
        binary.write_bytes(b'\x7fELF\x02\x01\x01' + bytes(11) + b'\x3e\x00')
        value = json.loads(original)
        value['contents'] = artifact.ssc.contents(self.bundle / artifact.PREFIX)
        metadata.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, 'ARM64'):
            artifact.package(self.bundle, self.dependency)

    def test_loader_checks_relocations_without_running_collector(self):
        for code, output in ((1, 'failure'), (0, 'not found'), (0, 'undefined symbol'), (0, '')):
            with patch.object(artifact.subprocess, 'run') as run:
                run.return_value.returncode = code
                run.return_value.stdout = output
                run.return_value.stderr = ''
                with self.assertRaisesRegex(ValueError, 'loader or relocations'):
                    artifact.check_loader(self.root)
                self.assertEqual(run.call_args.kwargs['env']['LD_TRACE_LOADED_OBJECTS'], '1')
