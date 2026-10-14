import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('basalt', ROOT / 'tools/build-basalt.py')
basalt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(basalt)


class BasaltSources(unittest.TestCase):
    def test_content_mode_and_links_are_bound(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            source.mkdir()
            header = source / 'thirdparty/vit/vit_interface.h'
            header.parent.mkdir(parents=True)
            header.write_text('interface\n')
            (source / 'data').mkdir()
            (source / 'other').mkdir()
            link = source / 'link'
            link.symlink_to('data', target_is_directory=True)
            profile = {'source_tree_sha256': basalt.tree_digest(source), 'vit_header_sha256': basalt.digest(header)}
            basalt.verify(source, profile)
            for change, restore in (
                (lambda: header.write_text('changed\n'), lambda: header.write_text('interface\n')),
                (lambda: header.chmod(0o755), lambda: header.chmod(0o644)),
                (lambda: (link.unlink(), link.symlink_to('other', target_is_directory=True)),
                 lambda: (link.unlink(), link.symlink_to('data', target_is_directory=True))),
            ):
                change()
                with self.assertRaises(ValueError):
                    basalt.verify(source, profile)
                restore()
                basalt.verify(source, profile)

    def test_git_metadata_does_not_change_build_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'source.c').write_text('source\n')
            before = basalt.tree_digest(root)
            (root / '.git').mkdir()
            (root / '.git/HEAD').write_text('repository metadata\n')
            self.assertEqual(before, basalt.tree_digest(root))

    def test_profile_pins_every_fetched_tree(self):
        profile = json.loads((ROOT / 'profiles/basalt.json').read_text())
        self.assertEqual(profile['sources'][0]['commit'], profile['commit'])
        self.assertEqual(len({item['path'] for item in profile['sources']}), len(profile['sources']))
        for item in profile['sources']:
            self.assertRegex(item['commit'], r'^[0-9a-f]{40}$')
            self.assertTrue(item['url'].startswith('https://'))
        for item in profile['archives']:
            self.assertRegex(item['sha256'], r'^[0-9a-f]{64}$')
            self.assertGreater(item['size'], 0)


if __name__ == '__main__':
    unittest.main()
