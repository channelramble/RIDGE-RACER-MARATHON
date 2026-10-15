#!/usr/bin/env python3
"""Pin, mirror and normalize Marathon's optional offline emulator bundle.

Only release CI needs network access, git, 7-Zip and (for AppImages) Linux x86-64.
Runtime import uses ordinary ZIP files; no external extractor or network is needed.
"""
from __future__ import annotations

import argparse
import fnmatch
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LIMIT = 1_950_000_000  # Leave margin below GitHub's per-release-asset limit.
STAMP = (2026, 1, 1, 0, 0, 0)


def run(*args, cwd=None):
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def write_json(path, obj):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(obj, indent=2) + '\n', encoding='utf-8')


def relative(value):
    p = PurePosixPath(value)
    if not value or '\\' in value or p.is_absolute() or '..' in p.parts or ':' in value:
        raise ValueError(f'Unsafe relative path: {value!r}')
    return p


def load_lock():
    lock = json.loads((ROOT / 'packaging/backend-lock.json').read_text())
    deps = ROOT / 'packaging/dependency-sources.json'
    if deps.exists():
        lock['sources'] += json.loads(deps.read_text())['sources']
    if lock['schema'] != 1:
        raise ValueError('Unsupported lock schema')
    ids = set()
    for source in lock['sources']:
        if source['id'] in ids:
            raise ValueError(f'Duplicate source {source["id"]}')
        ids.add(source['id'])
        if 'repo' in source and not re.fullmatch('[0-9a-f]{40}', source['commit']):
            raise ValueError(f'Source is not pinned: {source["id"]}')
        if 'url' in source and not re.fullmatch('[0-9a-f]{64}', source['sha256']):
            raise ValueError(f'Archive is not pinned: {source["id"]}')
    for binary in lock['binaries']:
        if not re.fullmatch('[0-9a-f]{64}', binary['sha256']):
            raise ValueError(f'Binary is not pinned: {binary["id"]}')
        if binary['source'] not in ids:
            raise ValueError(f'Missing source: {binary["source"]}')
    return lock


def download(url, digest, cache):
    cache.mkdir(parents=True, exist_ok=True)
    dest = cache / digest
    if dest.exists() and sha256(dest) == digest:
        return dest
    tmp = dest.with_suffix('.part')
    req = urllib.request.Request(url, headers={'User-Agent': 'Ridge-Racer-Marathon-release/1'})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=120) as response, tmp.open('wb') as out:
                shutil.copyfileobj(response, out, 1024 * 1024)
            actual = sha256(tmp)
            if actual != digest:
                raise ValueError(f'SHA256 mismatch for {url}: expected {digest}, got {actual}; refresh the reviewed lock, never accept rolling replacements automatically')
            tmp.replace(dest)
            return dest
        except OSError:
            if attempt == 2:
                raise
    raise RuntimeError('Download failed')


def validate_tree(root):
    base = root.resolve()
    for path in root.rglob('*'):
        if path.is_symlink():
            if not path.resolve().is_relative_to(base):
                raise ValueError(f'Archive contains an escaping symlink: {path}')
        elif not (path.is_file() or path.is_dir()):
            raise ValueError(f'Unsupported archive member: {path}')


def extract_zip(archive, dest):
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            rel = relative(info.filename.rstrip('/'))
            path = dest / str(rel)
            mode = info.external_attr >> 16
            if info.is_dir():
                path.mkdir(parents=True, exist_ok=True)
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.parent.is_symlink() or not path.parent.resolve().is_relative_to(dest.resolve()):
                raise ValueError('Symlink parent in ZIP')
            if stat.S_ISLNK(mode):
                target = z.read(info).decode('utf-8')
                if os.path.isabs(target) or not (path.parent / target).resolve().is_relative_to(dest.resolve()):
                    raise ValueError('Escaping ZIP symlink')
                path.symlink_to(target)
            else:
                with z.open(info) as src, path.open('wb') as out:
                    shutil.copyfileobj(src, out)
                path.chmod((mode & 0o777) or 0o644)


def extract(archive, fmt, dest):
    dest.mkdir(parents=True, exist_ok=True)
    if fmt == 'zip':
        extract_zip(archive, dest)
    elif fmt == 'appimage':
        if sys.platform != 'linux':
            raise RuntimeError('Normalize Linux AppImages on the Linux source/bundle runner')
        image = dest.parent / 'upstream.AppImage'
        shutil.copyfile(archive, image)
        image.chmod(0o755)
        run(image.resolve(), '--appimage-extract', cwd=dest)
        image.unlink()
        tree = dest / 'squashfs-root'
        if not tree.is_dir():
            # DwarFS/uruntime builds use APPIMAGE_EXTRACT_DIR or squashfs-root;
            # fail rather than publish a bundle with an unresolved entry point.
            raise RuntimeError('AppImage did not create squashfs-root')
        for child in list(tree.iterdir()):
            child.rename(dest / child.name)
        tree.rmdir()
    elif fmt.startswith('tar') or fmt in ('tgz', 'tbz2', 'txz'):
        with tarfile.open(archive) as tar:
            members = tar.getmembers()
            for member in members:
                relative(member.name.rstrip('/'))
                if member.isdev() or member.isfifo():
                    raise ValueError('Unsupported tar member')
                if member.issym() or member.islnk():
                    target = dest / member.name
                    resolved = (target.parent / member.linkname).resolve() if member.issym() else (dest / member.linkname).resolve()
                    if not resolved.is_relative_to(dest.resolve()):
                        raise ValueError('Escaping tar link')
            tar.extractall(dest, members=members, filter='data')
    elif fmt == '7z':
        exe = shutil.which('7zz') or shutil.which('7z')
        if not exe:
            raise RuntimeError('Install 7-Zip on the packaging runner')
        listing = subprocess.check_output([exe, 'l', '-slt', str(archive)], text=True)
        # First Path is the input archive; later Paths are archive members.
        for value in re.findall(r'^Path = (.+)$', listing, re.MULTILINE)[1:]:
            relative(value.rstrip('/'))
        run(exe, 'x', '-y', '-bd', '-o' + str(dest.resolve()), archive)
    else:
        raise ValueError(f'Unsupported archive format: {fmt}')
    validate_tree(dest)


def pack_zip(tree, archive):
    """Stable ZIP with POSIX mode bits and internal symlink targets preserved."""
    validate_tree(tree)
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
        for path in sorted(tree.rglob('*')):
            rel = path.relative_to(tree).as_posix()
            if path.is_dir() and not path.is_symlink():
                continue
            info = zipfile.ZipInfo(rel, STAMP)
            info.create_system = 3
            info.external_attr = path.lstat().st_mode << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            if path.is_symlink():
                z.writestr(info, os.readlink(path).encode())
            else:
                with path.open('rb') as src, z.open(info, 'w', force_zip64=True) as out:
                    shutil.copyfileobj(src, out, 1024 * 1024)
    if archive.stat().st_size > LIMIT:
        raise RuntimeError(f'Runtime ZIP exceeds release limit: {archive}')


def collect_licenses(tree, dest):
    count = 0
    for p in sorted(tree.rglob('*')):
        if '.git' in p.relative_to(tree).parts or not p.is_file() or p.is_symlink():
            continue
        name = p.name.lower()
        if name.startswith(('license', 'licence', 'copying', 'copyright', 'notice')) or 'legal' in p.relative_to(tree).parts:
            target = dest / p.relative_to(tree)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, target)
            count += 1
    return count


def checkout(source, dest):
    url = source.get('git_url', 'https://github.com/' + source['repo'] + '.git')
    if source.get('history'):
        run('git', 'clone', '--no-checkout', url, dest)
    else:
        dest.mkdir(parents=True)
        run('git', 'init', '--quiet', dest)
        run('git', '-C', dest, 'remote', 'add', 'origin', url)
        run('git', '-C', dest, 'fetch', '--depth=1', 'origin', source['commit'])
    run('git', '-C', dest, 'checkout', '--detach', source['commit'])
    run('git', '-c', 'url.https://github.com/.insteadOf=git@github.com:', '-C', dest, 'submodule', 'update', '--init', '--recursive', '--depth=1', '--jobs=4')
    actual = subprocess.check_output(['git', '-C', str(dest), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != source['commit']:
        raise ValueError('Source checkout does not match pin')
    submodules = subprocess.check_output(['git', '-C', str(dest), 'submodule', 'status', '--recursive'], text=True)
    if any(line.startswith(('-', '+', 'U')) for line in submodules.splitlines()):
        raise ValueError('Incomplete source submodule checkout')
    (dest / 'MARATHON-SOURCE.json').write_text(json.dumps(dict(source=source, submodules=submodules), indent=2) + '\n')


def source_tar(tree, target, keep_history=False):
    with target.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=0) as gz, tarfile.open(fileobj=gz, mode='w|') as tar:
        for path in sorted(tree.rglob('*')):
            rel = path.relative_to(tree)
            if not keep_history and '.git' in rel.parts:
                continue
            info = tar.gettarinfo(str(path), arcname=tree.name + '/' + rel.as_posix())
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            info.mtime = 0
            if info.isfile():
                with path.open('rb') as f:
                    tar.addfile(info, f)
            else:
                tar.addfile(info)


def split_asset(path):
    if path.stat().st_size <= LIMIT:
        return [path]
    parts = []
    remaining = path.stat().st_size
    with path.open('rb') as inp:
        number = 1
        while remaining:
            part = path.with_name(path.name + f'.part{number:03}')
            size = min(LIMIT, remaining)
            with part.open('wb') as out:
                written = 0
                while written < size:
                    chunk = inp.read(min(1024 * 1024, size - written))
                    if not chunk:
                        raise IOError('Unexpected end of source archive')
                    out.write(chunk)
                    written += len(chunk)
            remaining -= size
            parts.append(part)
            number += 1
    path.unlink()
    return parts


def build_sources(args, lock):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    entries = []
    old_index = output / 'source-manifest.json'
    previous = {s['id']: s for s in json.loads(old_index.read_text()).get('sources', [])} if old_index.exists() else {}
    for source in lock['sources']:
        sid = source['id']
        old = previous.get(sid)
        if old and all(old.get(key) == value for key, value in source.items()):
            good_files = old.get('files') and all((output / f['name']).is_file() and sha256(output / f['name']) == f['sha256'] for f in old['files'])
            good_notices = not old.get('license_files') or (output / 'licenses' / sid).is_dir()
            good_assets = not source.get('runtime_assets') or (output / 'runtime-data' / sid).is_dir()
            if good_files and good_notices and good_assets:
                print(f'Reusing verified source: {sid}', flush=True)
                entries.append(old)
                continue
        print(f'Source: {sid}', flush=True)
        with tempfile.TemporaryDirectory(prefix='rrm-source-', dir=args.work_dir) as temp:
            base = Path(temp)
            tree = base / sid
            if 'repo' in source:
                checkout(source, tree)
                archive = output / f'rrm-source-{sid}-{source["commit"][:12]}.tar.gz'
                source_tar(tree, archive, source.get('history', False))
            else:
                original = download(source['url'], source['sha256'], args.cache)
                suffix = ''.join(Path(source.get('filename', source['url'].split('?')[0])).suffixes) or '.archive'
                archive = output / f'rrm-source-{sid}{suffix}'
                shutil.copyfile(original, archive)
                fmt = source.get('archive') or ('zip' if suffix.endswith('.zip') else 'tar')
                if fmt in ('raw', 'patch', 'gz', 'none'):
                    tree.mkdir()
                    shutil.copyfile(original, tree / Path(source['url']).name)
                else:
                    extract(original, fmt, tree)
            n = collect_licenses(tree, output / 'licenses' / sid)
            if source.get('runtime_assets'):
                assets = tree / source['runtime_assets']
                dest = output / 'runtime-data' / sid
                shutil.copytree(assets, dest, dirs_exist_ok=True, symlinks=True)
            # Sources carry build recipes, complete submodules and their licences.
            parts = split_asset(archive)
            entries.append({**source, 'license_files': n, 'files': [{'name': p.name, 'sha256': sha256(p), 'size': p.stat().st_size} for p in parts]})
            write_json(output / 'source-manifest.json', {'schema': 1, 'sources': entries})
    write_json(output / 'source-manifest.json', {'schema': 1, 'sources': entries})
    (output / 'SOURCE-README.txt').write_text('Complete corresponding source and build recipes for the emulator bundle.\nEach Git snapshot includes recursive submodules at their pinned commits.\nThe PPSSPP FFmpeg history archive also retains historical commits used by prebuilt libraries.\nFor split files: concatenate .part001, .part002, ... in numeric order before extracting.\nSee source-manifest.json for provenance, SHA256 and source-to-backend mapping.\n')


def applicable(source, binary):
    return binary['id'] in source.get('backends', []) and (not source.get('platforms') or binary['platform'] in source['platforms'])


def find_executable(tree, pattern):
    matches = [p for p in tree.rglob('*') if p.is_file() and (fnmatch.fnmatch(p.relative_to(tree).as_posix(), pattern) or fnmatch.fnmatch(p.relative_to(tree).as_posix(), '*/' + pattern))]
    if len(matches) != 1:
        raise ValueError(f'Expected one executable matching {pattern!r}, found {[str(p) for p in matches]}')
    return matches[0]


def build_bundle(args, lock):
    index = json.loads(args.source_index.read_text())
    source_map = {s['id']: s for s in index['sources']}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    entries = []
    binaries = [b for b in lock['binaries'] if b['platform'] == args.platform]
    if not binaries:
        raise ValueError('No binaries for platform')
    for binary in binaries:
        print(f'Bundle: {binary["id"]} / {args.platform}', flush=True)
        sources = [s for s in lock['sources'] if applicable(s, binary)]
        for source in sources:
            if source['id'] not in source_map:
                raise ValueError(f'Missing mirrored source {source["id"]}')
            if any(source_map[source['id']].get(key) != value for key, value in source.items()):
                raise ValueError(f'Source manifest disagrees with lock: {source["id"]}')
        with tempfile.TemporaryDirectory(prefix='rrm-bundle-', dir=args.work_dir) as temp:
            tree = Path(temp) / 'payload'
            archive = download(binary['url'], binary['sha256'], args.cache)
            extract(archive, binary['archive'], tree)
            executable = find_executable(tree, binary['executable'])
            if binary.get('revision_marker'):
                if binary['revision_marker'].encode() not in executable.read_bytes():
                    raise ValueError('Core version does not match source commit')
            executable.chmod(executable.stat().st_mode | 0o111)
            for pattern in binary.get('remove', []):
                for p in list(tree.rglob('*')):
                    if p.is_file() and (fnmatch.fnmatch(p.relative_to(tree).as_posix(), pattern) or fnmatch.fnmatch(p.name, pattern.removeprefix('**/'))):
                        p.unlink()
            if binary.get('system_files'):
                assets = args.runtime_data / binary['source']
                if not assets.is_dir():
                    raise ValueError(f'Missing PPSSPP runtime assets: {assets}')
                shutil.copytree(assets, tree / 'system/PPSSPP', symlinks=True)
            notices = tree / 'MARATHON-LICENSES'
            for source in sources:
                src = args.licenses / source['id']
                if src.exists():
                    shutil.copytree(src, notices / source['id'], dirs_exist_ok=True)
            provenance = {'binary': binary, 'sources': [source_map[s['id']] for s in sources]}
            write_json(tree / 'MARATHON-PROVENANCE.json', provenance)
            (tree / 'MARATHON-SOURCE-NOTICE.txt').write_text('This emulator is redistributed by Ridge Racer Marathon under the upstream licence(s).\nLicence and copyright notices: MARATHON-LICENSES.\nExact source archives, including submodules and build recipes, are attached to the SAME GitHub release as this application:\nhttps://github.com/channelramble/RIDGE-RACER-MARATHON/releases\nSee MARATHON-PROVENANCE.json for the filenames and hashes.\nNo game disc, game ROM or console firmware is included.\n')
            target = output / (binary['id'] + '.zip')
            pack_zip(tree, target)
            entries.append({'id': binary['id'], 'version': binary['version'], 'archive': target.name, 'sha256': sha256(target), 'executable': executable.relative_to(tree).as_posix(), 'license': binary['license'], 'source_archive': [f['name'] for s in sources for f in source_map[s['id']]['files']], 'upstream_url': binary['url'], 'upstream_sha256': binary['sha256'], **({'system_files': binary['system_files']} if binary.get('system_files') else {})})
    write_json(output / 'manifest.json', {'schema': 1, 'platform': args.platform, 'entries': entries})
    write_json(output / 'source-manifest.json', index)
    shutil.copyfile(ROOT / 'packaging/backend-lock.json', output / 'backend-lock.json')
    verify_bundle(output)


def verify_bundle(folder):
    manifest = json.loads((folder / 'manifest.json').read_text())
    for entry in manifest['entries']:
        relative(entry['archive'])
        relative(entry['executable'])
        path = folder / entry['archive']
        if sha256(path) != entry['sha256']:
            raise ValueError('Bundle archive checksum mismatch')
        with zipfile.ZipFile(path) as z:
            bad = z.testzip()
            if bad:
                raise ValueError(f'ZIP CRC failed: {bad}')
            for name in z.namelist():
                relative(name.rstrip('/'))
            if entry['executable'] not in z.namelist():
                raise ValueError('Missing bundle executable')
            if 'MARATHON-PROVENANCE.json' not in z.namelist():
                raise ValueError('Missing binary/source provenance')
    print(f'Validated {len(manifest["entries"])} offline backends for {manifest["platform"]}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('check-lock')
    for command in ('sources', 'bundle'):
        p = sub.add_parser(command)
        p.add_argument('--output', type=Path, required=True)
        p.add_argument('--cache', type=Path, default=ROOT / '.bundle-cache')
        p.add_argument('--work-dir', type=Path)
        if command == 'bundle':
            p.add_argument('--platform', required=True, choices=['macos-arm64', 'windows-x86_64', 'linux-x86_64'])
            p.add_argument('--source-index', type=Path, required=True)
            p.add_argument('--licenses', type=Path, required=True)
            p.add_argument('--runtime-data', type=Path, required=True)
    p = sub.add_parser('verify')
    p.add_argument('folder', type=Path)
    args = parser.parse_args()
    if args.command == 'verify':
        verify_bundle(args.folder)
        return
    lock = load_lock()
    if args.command == 'check-lock':
        print(f'Validated {len(lock["binaries"])} binary pins and {len(lock["sources"])} source pins')
    elif args.command == 'sources':
        build_sources(args, lock)
    else:
        build_bundle(args, lock)


if __name__ == '__main__':
    main()
