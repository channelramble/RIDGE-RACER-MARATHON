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
| RR6 native recomp | Included; macOS 14+ | Included | Included; experimental |
| Fukami / Ridge Racer V recomp | Included; macOS 27+ | Included | Included |
| R4 recomp setup kit | Included; builds from your disc | Included; builds from your disc | Upstream download only; no redistribution license |
| Rave Racer native | Source build and Link | Included | Included |
| Ridge Racer PS1 recomp | Source build and Link | Source build and Link | No catalog build |
| Rage Racer PC | Upstream download only | Upstream download only | Upstream download only |

Game discs, ROMs and Sony BIOS/firmware are not included. Supply them through the
launcher's normal setup. Native runtime packages require your own game data;
bundling the runtime does not include a playable copy of the game. RR6 on Apple
Silicon uses the tested community build from the contributor fork linked in the
lock, based on already merged upstream source. It is ad-hoc signed, not notarized.

Rage Racer PC's `LICENSE.md` reserves copyright, and the Linux R4 build publishes
no redistribution license. Those two downloads remain upstream-only. RR1 publishes
source, and Rave Racer has no packaged Mac build; those require a local build.
The R4 setup kit is included but still downloads its compiler tools and generates
the game from your disc on first run. These are specific exceptions, not a blanket
exclusion of native ports. Some Windows backends require Microsoft's Visual C++ x64 runtime.
Run `Install Windows Runtime.cmd` beside the Windows launcher if it is missing.
The helper downloads a pinned installer directly from Microsoft, checks SHA256
and Microsoft's signature, and opens the normal interactive installer. This
optional step requires internet access; the installer is not redistributed.
Microsoft's [redistribution terms](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files)
reserve redistribution rights to licensed Visual Studio users. macOS PCSX2 needs
Rosetta 2. A graphics driver remains an operating-system
requirement. Linux PCSX2 and RPCS3 also use the desktop OpenGL dispatcher
`libOpenGL.so.0`. On Ubuntu/Debian, install it with `sudo apt install libopengl0`
if missing; other distributions provide it through their libglvnd/OpenGL package.
The release keeps GPU drivers under the operating system's package management.

The package preserves upstream applications, their support files and their
licenses. Linux AppImages are extracted during release packaging and launched
through their `AppRun` entry point, so the packaged copies do not need FUSE.
Upstream macOS application signatures are kept intact in the packaged payloads.
On first import, Marathon gives its writable PPSSPP copy a separate bundle ID and
signs that copy ad hoc, just as its online installer does. This isolates PPSSPP's
preferences from any copy the user already has. Third-party licenses and
provenance are placed beside the applications, outside signed bundles.

## Reproducible release inputs

[`packaging/backend-lock.json`](../packaging/backend-lock.json) pins every runtime
archive by SHA256 and connects it to an exact source commit. The Libretro server
uses rolling URLs: the lock additionally records each core's embedded revision.
A changed nightly archive fails packaging instead of silently replacing a
reviewed build. Released application bundles contain their pinned payloads and
remain usable independently of those rolling URLs.

[`packaging/dependency-sources.json`](../packaging/dependency-sources.json) and
[`packaging/linux-dependency-sources.json`](../packaging/linux-dependency-sources.json) record
third-party source archives and repositories, their exact hashes, and the
platform/backend combinations that use them. Source provenance is recorded in
`source-manifest.json`; each installed emulator also includes
`MARATHON-PROVENANCE.json`, `MARATHON-SOURCE-NOTICE.txt`, and
`MARATHON-LICENSES/`.

Source archives are attached to the **same GitHub release** as the applications.
Git snapshots include recursive submodules at their recorded commits, vendored
libraries, build scripts, patches and notices. Linux dependency records include
exact distribution source packages and packaging recipes matched to the bundled
libraries. The PPSSPP FFmpeg archive retains
Git history because upstream binaries use prebuilt libraries from historical
commits. Dependency archives are mirrored as their original verified downloads.
No external source URL is used as a substitute for an attached source archive.

Each release asset stays below 1.95 GB. When a source archive exceeds that limit,
the builder emits numbered `.part001`, `.part002`, … files. Concatenate those
parts in numeric order before extracting. The manifest records a SHA256 for each
part. Runtime ZIPs exceeding the limit fail the build.

## Building or checking a bundle

Use Python 3.12+, Git, Subversion, zstd, and 7-Zip. Normalize Linux binaries on a Linux x86-64
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
