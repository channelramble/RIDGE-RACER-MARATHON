# Complete desktop release bundles

The desktop release contains the launcher and a local, checksum-verified emulator
bundle. On first launch, Marathon imports each available backend into its writable
application-data directory. Importing does not download an emulator or require
7-Zip. Existing user installations, settings, saves and firmware stay in place.

| Backend | macOS Apple Silicon | Windows x64 | Linux x64 |
| --- | --- | --- | --- |
| SwanStation core | Included | Included | Included |
| PCSX-ReARMed core | Included | Included | Included |
| PPSSPP core and its support assets | Included | Included | Included |
| MAME core | Included | Included | Included |
| PPSSPP standalone | Included | Included | Included |
| PCSX2 standalone | Included; requires Rosetta 2 | Included | Included |
| RPCS3 standalone | Included | Included | Included |
| MAME standalone | Link an installed copy | Included | Link an installed copy |
| Xenia Canary | No upstream macOS build | Included | Included; experimental |

Game discs, ROMs, Sony BIOS/firmware, and game-derived native executables are not
included. Supply the relevant games and firmware through the launcher's normal
setup. Native ports and recompilation projects remain available through the
catalog's Install or Link actions. Their upstream distributions and requirements
still apply. Some Windows emulators require Microsoft's Visual C++ x64 runtime;
macOS PCSX2 needs Rosetta 2. A graphics driver remains an operating-system
requirement.

The package preserves upstream applications, their support files and their
licenses. Linux AppImages are extracted during release packaging and launched
through their `AppRun` entry point, so the packaged copies do not need FUSE.
Upstream macOS application signatures are kept intact. Third-party licenses and
provenance are placed beside the applications, outside signed bundles.

## Reproducible release inputs

[`packaging/backend-lock.json`](../packaging/backend-lock.json) pins every runtime
archive by SHA256 and connects it to an exact source commit. The Libretro server
uses rolling URLs: the lock additionally records each core's embedded revision.
A changed nightly archive fails packaging instead of silently replacing a
reviewed build. Released application bundles contain their pinned payloads and
remain usable independently of those rolling URLs.

[`packaging/dependency-sources.json`](../packaging/dependency-sources.json) records
third-party source archives and repositories, their exact hashes, and the
platform/backend combinations that use them. Source provenance is recorded in
`source-manifest.json`; each installed emulator also includes
`MARATHON-PROVENANCE.json`, `MARATHON-SOURCE-NOTICE.txt`, and
`MARATHON-LICENSES/`.

Source archives are attached to the **same GitHub release** as the applications.
Git snapshots include recursive submodules at their recorded commits, vendored
libraries, build scripts, patches and notices. The PPSSPP FFmpeg archive retains
Git history because upstream binaries use prebuilt libraries from historical
commits. Dependency archives are mirrored as their original verified downloads.
No external source URL is used as a substitute for an attached source archive.

Each release asset stays below 1.95 GB. When a source archive exceeds that limit,
the builder emits numbered `.part001`, `.part002`, … files. Concatenate those
parts in numeric order before extracting. The manifest records a SHA256 for each
part. Runtime ZIPs exceeding the limit fail the build.

## Building or checking a bundle

Use Python 3.12+, Git, and 7-Zip. Normalize Linux binaries on a Linux x86-64
machine, since their AppImage extraction utilities run during packaging. The
release workflow performs these steps before packaging and signing the launcher:

```sh
python3 scripts/bundle-backends.py check-lock
python3 -m unittest discover -s tests/packaging -v
python3 scripts/bundle-backends.py sources \
  --output dist/sources --cache .bundle-cache
python3 scripts/bundle-backends.py bundle \
  --platform linux-x86_64 --output dist/offline-bundle \
  --source-index dist/sources/source-manifest.json \
  --licenses dist/sources/licenses \
  --runtime-data dist/sources/runtime-data --cache .bundle-cache
python3 scripts/bundle-backends.py verify dist/offline-bundle
RRM_BUNDLE_DIR="$PWD/dist/offline-bundle" bash scripts/build-linux.sh
```

Other platform keys are `macos-arm64` and `windows-x86_64`. Sources may require
substantial temporary disk space, particularly RPCS3's LLVM submodule. The source
builder deletes each temporary checkout after archiving it. Re-running with the
same output directory reuses archives only after verifying their hashes and
checking their notices and runtime assets are present.

The bundle manifest is `resources/bundle/manifest.json` (or the equivalent
platform resources directory). Its schema is 1, its `platform` must match the
running application, and every entry names a relative ZIP path, SHA256 and exact
executable path. Optional `system_files` mappings merge support assets into the
application-data system directory. The importer preserves existing user files.

Normalized ZIPs preserve Unix executable modes and internal framework symlinks.
The builder rejects paths or links escaping their destination and verifies ZIP
CRC, archive hashes, core revision markers and executable presence. Release
smoke checks exercise the application's actual offline importer as well.
