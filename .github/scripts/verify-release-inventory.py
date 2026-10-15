#!/usr/bin/env python3
"""Check the complete release against its reviewed lock before uploading it."""
import argparse
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bundle', ROOT / 'scripts/bundle-backends.py')
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


def verify(folder):
    lock = bundle.load_lock()
    index = json.loads((folder / 'source-manifest.json').read_text())
    sources = {source['id']: source for source in index['sources']}
    assert len(sources) == len(index['sources']), 'Duplicate source manifest entries'
    assert set(sources) == {source['id'] for source in lock['sources']}, 'Source inventory differs from lock'
    expected = {'RidgeRacerMarathon-macos-arm64.zip',
                'RidgeRacerMarathon-windows-x64.zip',
                'RidgeRacerMarathon-linux-x64.tar.gz',
                'source-manifest.json', 'SOURCE-README.txt'}
    recorded_hashes = {}
    for source in lock['sources']:
        mirrored = sources[source['id']]
        assert all(mirrored.get(key) == value for key, value in source.items()), source['id']
        assert mirrored['files'], f'No source archives: {source["id"]}'
        for item in mirrored['files']:
            name = item['name']
            assert name == Path(name).name and name.startswith('rrm-source-'), name
            assert name not in recorded_hashes, f'Duplicate source asset: {name}'
            path = folder / name
            assert path.stat().st_size == item['size'], f'Source size mismatch: {name}'
            recorded_hashes[name] = item['sha256']
            expected.add(name)
    actual = {path.name for path in folder.iterdir() if path.name != 'SHA256SUMS.txt'}
    assert actual == expected, f'Release inventory mismatch: missing={expected - actual}; extra={actual - expected}'
    checksums = []
    for name in sorted(expected):
        path = folder / name
        assert path.is_file() and 0 < path.stat().st_size < 2_000_000_000, f'Invalid release asset: {name}'
        digest = bundle.sha256(path)
        if name in recorded_hashes:
            assert digest == recorded_hashes[name], f'Source checksum mismatch: {name}'
        checksums.append(f'{digest}  {name}\n')
    (folder / 'SHA256SUMS.txt').write_text(''.join(checksums))
    print(f'Verified 3 runtime packages, {len(sources)} source inputs and {len(expected)} release assets.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    verify(parser.parse_args().folder)
