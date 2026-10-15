# Building

Marathon is one CMake project (C++20, SDL3, Dear ImGui, OpenGL / OpenGL ES).

## macOS (Apple Silicon) and Linux

```sh
brew install cmake ninja sdl3          # Linux: your distro's cmake, ninja and SDL3 (or let CMake build SDL)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
open "build/Ridge Racer Marathon.app"  # Linux: ./build/RidgeRacerMarathon
```

Without a system SDL3, CMake downloads and builds SDL `release-3.4.18` statically
(`-DRRM_VENDORED_SDL=ON` forces that).

## Windows x64

Cross-compiled from macOS or Linux with mingw-w64:

```sh
brew install mingw-w64 ninja           # Ubuntu: sudo apt install mingw-w64 ninja-build zip
scripts/build-windows.sh               # -> dist/RidgeRacerMarathon-windows-x64.zip
```

The script links SDL statically and checks that the exe imports system DLLs only.
A native MSVC build should work with plain CMake (`cmake -S . -B build && cmake --build build --config Release`); CI builds it on every push.

## Release packages

One script per desktop package. Each builds a Release configuration with SDL linked statically and
without the developer tools, writes to `dist/`, and puts `LICENSE`, `THIRD_PARTY_NOTICES.md` and
`LICENSES/` (the full third-party licence texts) in the package; the font licences travel with the
fonts in `assets/fonts/`.

| Script | Runs on | Produces |
|---|---|---|
| `scripts/build-macos.sh` | macOS, Apple Silicon | `RidgeRacerMarathon-macos-arm64.zip`: a `RidgeRacerMarathon` folder with the signed `Ridge Racer Marathon.app` and the licence files (also inside the app, in `Contents/Resources`) |
| `scripts/build-windows.sh` | macOS or Linux with mingw-w64 | `RidgeRacerMarathon-windows-x64.zip`: the exe with `catalog/`, `assets/` and the licence files |
| `scripts/build-linux.sh` | Linux x64 (arm64 gives `linux-arm64`) | `RidgeRacerMarathon-linux-x64.tar.gz`: CMake's install layout (`bin/`, `share/` with the desktop entry, icon and `share/doc/ridge-racer-marathon/`) plus the licence files at the top |

Environment variables:

| Variable | Meaning |
|---|---|
| `BUILD_DIR` | Build folder (default `build-dist/<platform>`) |
| `DIST_DIR` | Output folder (default `dist/`) |
| `JOBS` | Parallel build jobs (default: number of CPUs) |
| `RRM_SDL_SOURCE_DIR` | An existing SDL checkout at `release-3.4.18`, instead of downloading SDL |
| `RRM_CMAKE_ARGS` | Extra CMake configure arguments |
| `CODESIGN_IDENTITY` | macOS: signing identity, e.g. `Developer ID Application: Name (TEAMID)`. Unset: ad-hoc signature |
| `NOTARY_PROFILE` | macOS: `notarytool` keychain profile; when set, the app is notarized and the ticket stapled (needs `CODESIGN_IDENTITY`) |
| `MACOS_DEPLOYMENT_TARGET` | macOS: oldest supported version (default `11.0`, as in `Info.plist`) |

macOS signing:

- **Ad-hoc (default):** fine on the Mac that built it. Other Macs block the first launch of a
  downloaded copy; allow it under System Settings > Privacy & Security > Open Anyway.
- **Developer ID:** `CODESIGN_IDENTITY="Developer ID Application: Name (TEAMID)" scripts/build-macos.sh`
  signs `librrm_present.dylib` and the app with the hardened runtime and a secure timestamp. The app
  gets three entitlements because libretro cores run inside it: JIT memory
  (`allow-jit`, `allow-unsigned-executable-memory`) and loading cores signed by someone else
  (`disable-library-validation`).
- **Notarization:** store the credentials once, then pass the profile name:

  ```sh
  xcrun notarytool store-credentials marathon-notary --apple-id you@example.com --team-id TEAMID
  CODESIGN_IDENTITY="Developer ID Application: Name (TEAMID)" NOTARY_PROFILE=marathon-notary scripts/build-macos.sh
  ```

  The script submits the app, waits for Apple's answer, staples the ticket and checks the result
  with `spctl`.

Every macOS run checks the signature with `codesign --verify --strict`, both on the app and on
the copy inside the zip, and checks that the app links system libraries only.

Linux needs the headers SDL builds its X11, Wayland and audio support from (on Ubuntu,
`.github/scripts/install-linux-deps.sh` installs them). SDL opens those libraries at runtime, so
the binary only depends on the C/C++ runtime; build on the oldest distro you want to support,
because the binary needs at least the glibc it was built with (the script prints that version).
Run `RidgeRacerMarathon/bin/RidgeRacerMarathon` from the extracted folder, or copy `bin/` and
`share/` into `~/.local` to get a menu entry.

When the SDL version changes, copy its new licence texts into `LICENSES/SDL3*.txt`; the scripts
warn when they differ from the SDL that was built.

## Android and iOS

Not buildable yet. The CMake branches exist (GLES 3, bundled cores as embedded
frameworks on iOS), but the Gradle project under `platform/android/` (only
launcher icons so far), `platform/ios/Info.plist.in` and the platform glue for
handing games to other Android apps (`launchExternalApp` / `isAppInstalled`)
are still to be written.

## Developer tools

Built with the main project unless `-DRRM_BUILD_TESTS=OFF`:

| Tool | What it does |
|---|---|
| `launch_tool` | Drives the real backend adapters without the UI. `matrix <games folder>` prints, for every recognised game and every way to run it on this OS, the command line or core options Marathon would use (dry run: nothing is written or started). `run <folder> <game> <backend>` plays an in-app core in a hidden window and saves a PNG. `--install <backend>` runs the real installer, `--spawn N` really starts external programs. See the header of `tests/launch_tool.cpp`. |
| `lr_smoke` | Runs any libretro core + content in the in-app host and saves a PNG (core options, button presses, save states). |
| `scan_tool` | Identifies a folder of dumps, verifies hashes, rebuilds MAME sets. |
| `fg_eval` (macOS) | Scores the Metal frame generation offline: `fg_eval <out dir> <frame.png>...` rebuilds the middle of each three consecutive game frames from the outer two and prints PSNR against the real one (next to a blend and a repeat), plus GPU time per game frame and per generated image. |

On macOS the build also produces `librrm_present.dylib` (`tools/framegrab`), copied into the app's
`Contents/Frameworks`: Marathon injects it into the RR6 recomp and RPCS3 for frame generation, and the
same library captures frames for tests (`RRM_GRAB_*`, `RRM_FRAMEGEN_DUMP`, `RRM_KEYS`; see the header of
`tools/framegrab/framegrab.mm`). `RRM_PRESENT_LIBRARY` points Marathon at another copy.

Scanner unit tests: `cmake -S tests/scanner -B build-scanner && cmake --build build-scanner && build-scanner/scanner_unit_tests`.

Every test tool uses its own data folder (`--data`, default a fresh temp folder), so it never touches a real
Marathon install. Linked recomp builds are written to when a game really starts (their settings live beside
them), so test against copies of your builds, never the originals.
