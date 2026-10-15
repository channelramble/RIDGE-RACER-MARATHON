"""Release integrity tests; no network, game data or emulator execution required."""
import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bundle', ROOT / 'scripts/bundle-backends.py')
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class BundleTest(unittest.TestCase):
    def test_lock_has_exact_offline_source_mapping(self):
        lock = bundle.load_lock()
        keys = [(b['id'], b['platform']) for b in lock['binaries']]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertEqual(len({b['id'] for b in lock['binaries']}), 9)
        self.assertEqual({b['platform'] for b in lock['binaries']}, {'macos-arm64', 'windows-x86_64', 'linux-x86_64'})
        for binary in lock['binaries']:
            source = next(s for s in lock['sources'] if s['id'] == binary['source'])
            self.assertIn(binary['id'], source['backends'])
            self.assertTrue(binary['url'].startswith('https://'))
            self.assertGreater(binary['size'], 0)

    def test_runtime_zip_preserves_executable_and_framework_link(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            tree = base / 'tree'
            exe = tree / 'Example.app/Contents/Frameworks/Foo.framework/Versions/A/Foo'
            exe.parent.mkdir(parents=True)
            exe.write_bytes(b'example executable')
            exe.chmod(0o755)
            link = tree / 'Example.app/Contents/Frameworks/Foo.framework/Foo'
            link.symlink_to('Versions/A/Foo')
            first = base / 'first.zip'
            second = base / 'second.zip'
            bundle.pack_zip(tree, first)
            os.utime(exe, (123456, 123456))
            bundle.pack_zip(tree, second)
            self.assertEqual(bundle.sha256(first), bundle.sha256(second))
            dest = base / 'output'
            dest.mkdir()
            bundle.extract_zip(first, dest)
            restored = dest / link.relative_to(tree)
            self.assertTrue(restored.is_symlink())
            self.assertEqual(restored.read_bytes(), b'example executable')
            self.assertEqual((dest / exe.relative_to(tree)).stat().st_mode & 0o777, 0o755)

    def test_zip_rejects_path_traversal_and_escaping_symlinks(self):
        for member, target in [('../outside', None), ('/absolute', None), ('C:/file', None), ('evil', '../../outside')]:
            with self.subTest(member=member), tempfile.TemporaryDirectory() as temp:
                base = Path(temp)
                zpath = base / 'bad.zip'
                with zipfile.ZipFile(zpath, 'w') as z:
                    info = zipfile.ZipInfo(member)
                    if target:
                        info.create_system = 3
                        info.external_attr = (stat.S_IFLNK | 0o777) << 16
                    z.writestr(info, target or 'bad')
                out = base / 'out'
                out.mkdir()
                with self.assertRaises(ValueError):
                    bundle.extract_zip(zpath, out)

    def test_verify_rejects_modified_archive(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            tree = base / 'tree'
            tree.mkdir()
            (tree / 'run').write_text('binary')
            (tree / 'MARATHON-PROVENANCE.json').write_text('{}')
            archive = base / 'test.zip'
            bundle.pack_zip(tree, archive)
            bundle.write_json(base / 'manifest.json', {'schema': 1, 'platform': 'linux-x86_64', 'entries': [{'id': 'test', 'archive': 'test.zip', 'sha256': bundle.sha256(archive), 'executable': 'run'}]})
            bundle.verify_bundle(base)
            archive.write_bytes(archive.read_bytes() + b'tamper')
            with self.assertRaises(ValueError):
                bundle.verify_bundle(base)


if __name__ == '__main__':
    unittest.main()
