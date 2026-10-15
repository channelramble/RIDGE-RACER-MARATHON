"""Release integrity tests; no network, game data or emulator execution required."""
import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bundle', ROOT / 'scripts/bundle-backends.py')
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class BundleTest(unittest.TestCase):
    def test_source_cache_prunes_obsolete_archives_only_after_success(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            upstream = base / 'upstream.txt'
            upstream.write_text('source fixture')
            output = base / 'output'
            output.mkdir()
            stale = output / 'rrm-source-previous.tar.gz'
            stale.write_text('previous source archive')
            unrelated = output / 'unrelated.txt'
            unrelated.write_text('retain me')
            unrelated_dir = output / 'rrm-source-unrelated-directory'
            unrelated_dir.mkdir()
            args = SimpleNamespace(output=output, cache=base / 'cache', work_dir=None)
            source = {'id': 'fixture', 'url': upstream.as_uri(), 'sha256': '0' * 64,
                      'archive': 'none', 'filename': upstream.name}
            with self.assertRaises(ValueError):
                bundle.build_sources(args, {'sources': [source]})
            self.assertTrue(stale.exists())
            source['sha256'] = bundle.sha256(upstream)
            bundle.build_sources(args, {'sources': [source]})
            self.assertFalse(stale.exists())
            self.assertTrue(unrelated.exists())
            self.assertTrue(unrelated_dir.is_dir())
            manifest = json.loads((output / 'source-manifest.json').read_text())
            for entry in manifest['sources']:
                for item in entry['files']:
                    self.assertEqual(bundle.sha256(output / item['name']), item['sha256'])

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

    @unittest.skipIf(os.name == 'nt', 'POSIX framework bundles are assembled on macOS/Linux')
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

    def test_prerequisite_download_metadata_is_separate_and_verified(self):
        lock = bundle.load_lock()
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            self.assertEqual(bundle.build_prerequisites(base, lock, 'macos-arm64'), [])
            prerequisites = bundle.build_prerequisites(base, lock, 'windows-x86_64')
            self.assertEqual(len(prerequisites), 1)
            self.assertEqual(prerequisites[0]['mode'], 'download')
            self.assertEqual(list(base.rglob('*.exe')), [])
            bundle.write_json(base / 'manifest.json', {'schema': 1, 'platform': 'windows-x86_64', 'entries': [], 'prerequisites': prerequisites})
            bundle.verify_bundle(base)
            descriptor = base / prerequisites[0]['descriptor']
            descriptor.write_text(descriptor.read_text() + ' ')
            with self.assertRaises(ValueError):
                bundle.verify_bundle(base)

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
