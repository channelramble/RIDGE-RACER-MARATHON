#!/usr/bin/env python3
"""TEST_ONLY: probe real runtime archives with synthetic source metadata.

Never publish outputs from this script. It uses the production normalizer and
importer, but intentionally substitutes empty source archives/licenses so that
runtime extraction issues can be found independently of the source mirror job.
All downloads, test bundles and application-data directories are temporary.
No game, firmware, setup dialog or runtime installer is executed. Standalone
AppRun launchers are tested only with source-confirmed version-exit switches.
"""
from __future__ import annotations

import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import traceback
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def load_core(path, expected):
    class Info(ctypes.Structure):
        _fields_ = [('name', ctypes.c_char_p), ('version', ctypes.c_char_p),
                    ('extensions', ctypes.c_char_p), ('need_fullpath', ctypes.c_bool),
                    ('block_extract', ctypes.c_bool)]
    library = ctypes.CDLL(str(path))
    library.retro_get_system_info.argtypes = [ctypes.POINTER(Info)]
    library.retro_get_system_info.restype = None
    info = Info()
    library.retro_get_system_info(ctypes.byref(info))
    if not info.name or not info.version:
        raise RuntimeError('Missing libretro metadata')
    version = info.version.decode()
    if expected not in version:
        raise RuntimeError(f'Core version differs from pin: {version!r}, expected {expected!r}')
    print(f'CORE PASS: {info.name.decode()} / {version}', flush=True)



def smoke_apprun(backend, executable, work):
    """Run only source-confirmed version switches, never emulator setup/gameplay."""
    switches = {'ppsspp': (['--version'], '1.20.4', {0}),
                # QtHost returns EXIT_FAILURE after both -help and -version.
                'pcsx2': (['-version'], '2.8.2', {1}),
                'rpcs3': (['--headless', '--version'], '4d5def58', {0})}
    arguments, expected, allowed = switches[backend]
    environment = os.environ.copy()
    for key in ('APPIMAGE', 'APPDIR', 'ARGV0', 'LD_PRELOAD'):
        environment.pop(key, None)
    for kind in ('CONFIG', 'DATA', 'CACHE', 'STATE', 'RUNTIME'):
        directory = work / ('xdg-' + kind.lower())
        directory.mkdir(mode=0o700)
        environment['XDG_' + kind + ('_DIR' if kind == 'RUNTIME' else '_HOME')] = str(directory)
    environment['DISABLE_AUTO_UPDATES'] = '1'
    environment['QT_QPA_PLATFORM'] = 'xcb'
    command = ['xvfb-run', '-a', str(executable), *arguments]
    result = subprocess.run(command, cwd=work, env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors='replace', timeout=45)
    print(f'APPRUN {backend} exit={result.returncode}:\n{result.stdout}', flush=True)
    if result.returncode not in allowed or expected not in result.stdout:
        raise RuntimeError(f'{backend} extracted AppRun failed its version smoke check')


def support_assets(lock, runtime_data):
    source = next(s for s in lock['sources'] if s['id'] == 'ppsspp-libretro')
    prefix = f'ppsspp-{source["commit"]}/assets/'
    output = runtime_data / source['id']
    output.mkdir(parents=True)
    url = f'https://codeload.github.com/{source["repo"]}/tar.gz/{source["commit"]}'
    with urllib.request.urlopen(url, timeout=120) as response, tarfile.open(fileobj=response, mode='r|gz') as archive:
        for member in archive:
            if member.name.startswith(prefix) and member.name[len(prefix):]:
                member.name = member.name[len(prefix):]
                archive.extract(member, output, filter='data')
    if not (output / 'ppge_atlas.zim').is_file():
        raise RuntimeError('Pinned PPSSPP support assets are incomplete')


def probe(args):
    if sys.platform != 'linux' or os.uname().machine != 'x86_64':
        raise RuntimeError('This probe needs Linux x86-64')
    tool = args.bundle_tool.resolve()
    if not tool.is_file():
        raise RuntimeError(f'Build bundle_tool first: {tool}')
    spec = importlib.util.spec_from_file_location('release_bundle', ROOT / 'scripts/bundle-backends.py')
    bundle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bundle)
    lock = bundle.load_lock()
    binaries = [b for b in lock['binaries'] if b['platform'] == 'linux-x86_64']
    if len(binaries) != 8:
        raise RuntimeError(f'Expected eight Linux backend payloads, found {len(binaries)}')
    results = []
    with tempfile.TemporaryDirectory(prefix='rrm-TEST_ONLY-DO_NOT_PUBLISH-', dir=os.environ.get('RUNNER_TEMP')) as tmp:
        work = Path(tmp)
        cache = work / 'downloads'
        licenses = work / 'EMPTY_TEST_LICENSES'
        licenses.mkdir()
        runtime_data = work / 'runtime-data'
        support_assets(lock, runtime_data)
        source_index = work / 'TEST_ONLY-source-index.json'
        bundle.write_json(source_index, {'schema': 1, 'TEST_ONLY_DO_NOT_PUBLISH': True,
                          'sources': [{**s, 'files': [], 'license_files': 0} for s in lock['sources']]})
        for binary in binaries:
            backend = binary['id']
            print(f'\n===== TEST_ONLY Linux payload: {backend} =====', flush=True)
            try:
                with tempfile.TemporaryDirectory(prefix=backend + '-', dir=work) as iteration:
                    case = Path(iteration)
                    output = case / 'TEST_ONLY-bundle'
                    data = case / 'isolated-user-data'
                    if args.input_cache:
                        frozen = args.input_cache / binary['sha256']
                        if frozen.is_file():
                            cache.mkdir(parents=True, exist_ok=True)
                            shutil.copyfile(frozen, cache / binary['sha256'])
                    bundle.build_bundle(argparse.Namespace(platform='linux-x86_64', output=output,
                        source_index=source_index, licenses=licenses, runtime_data=runtime_data,
                        cache=cache, work_dir=case), {**lock, 'binaries': [binary]})
                    subprocess.run([str(tool), '--bundle', str(output), '--data', str(data),
                                    '--catalog', str(ROOT / 'catalog')], check=True, timeout=180)
                    library = json.loads((data / 'library.json').read_text())
                    if library['games'] or library['system']:
                        raise RuntimeError('Fresh test data unexpectedly contains games or firmware')
                    installed = library['installs'][backend]
                    executable = Path(installed['executable'])
                    if not executable.is_file() or not executable.resolve().is_relative_to(data.resolve()):
                        raise RuntimeError('Imported executable is missing or escaped test data')
                    if not os.access(executable, os.X_OK):
                        raise RuntimeError('Imported executable lost its execute bit')
                    if backend.startswith('lr-'):
                        subprocess.run([sys.executable, __file__, '--load-core', str(executable),
                                        '--expected', binary['revision_marker']], check=True, timeout=60)
                    if backend == 'lr-ppsspp' and not (data / 'system/libretro/PPSSPP/ppge_atlas.zim').is_file():
                        raise RuntimeError('PPSSPP support assets did not import')
                    if backend in ('ppsspp', 'pcsx2', 'rpcs3'):
                        smoke_apprun(backend, executable, case)
                    print(f'PASS: {backend}: pinned archive, normalization, ZIP, offline import and executable', flush=True)
                    results.append({'id': backend, 'ok': True})
            except Exception as error:
                traceback.print_exc()
                results.append({'id': backend, 'ok': False, 'error': str(error)})
            finally:
                # Do not retain inputs across payloads or upload synthetic bundles.
                if cache.exists():
                    shutil.rmtree(cache)
        print('\nTEST_ONLY RESULTS\n' + json.dumps(results, indent=2), flush=True)
    print('All downloaded payloads, synthetic bundles and imported test data removed.', flush=True)
    if any(not result['ok'] for result in results):
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle-tool', type=Path)
    parser.add_argument('--input-cache', type=Path, help='Optional previously verified SHA-named inputs')
    parser.add_argument('--load-core', type=Path)
    parser.add_argument('--expected')
    args = parser.parse_args()
    if args.load_core:
        if not args.expected:
            parser.error('--load-core requires --expected')
        load_core(args.load_core, args.expected)
    else:
        if not args.bundle_tool:
            parser.error('--bundle-tool is required')
        probe(args)


if __name__ == '__main__':
    main()
