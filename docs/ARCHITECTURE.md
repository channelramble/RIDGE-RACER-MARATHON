# Architecture

Marathon is one C++20 program (SDL3 + Dear ImGui + OpenGL / OpenGL ES) built
from a single CMake project for macOS, Windows and Linux; the Android and iOS
branches exist but are not buildable yet (see [BUILDING.md](BUILDING.md)).

```
src/main.cpp            SDL main callbacks (same entry on every OS)
src/app/                UI and application glue
  App.*                 window, GL context, jobs, launching, persistence
  UiCommon.cpp          theme, widgets, the unified settings editor, dialogs
  UiScreens.cpp         library, game page, add games, backends, settings, about
  UiPlayer.cpp          in-app player: game view, pause menu, hotkeys, touch pad
src/core/               no UI
  Catalog.*             catalog/games.json + backends.json + recomps.json, plus extra files in <data dir>/catalog (docs/CATALOG.md)
  Library.*             the user's files, installs, system files, per-game settings
  Settings.*            unified enhancement settings (resolution, fps, speed, redraw...)
  Scanner.* formats/    recognise dumps by content (SYSTEM.CNF, PARAM.SFO, XEX, MAME CRCs)
  RomSet.*              rebuild MAME sets from loose/old-named ROMs by CRC
  Process.* Net.*       child processes, downloads, GitHub releases, archive extraction
  ConfigFiles.*         comment-preserving INI / YAML / TOML editors for backend configs
src/backends/           one adapter per backend (see Adapters.h)
src/libretro/           in-process libretro host (PS1, PSP and arcade cores)
tools/framegrab/        macOS present library: frame generation and frame capture for
                        external backends (librrm_present.dylib), fg_eval
tests/                  developer tools (launch_tool, scan_tool, lr_smoke, recomp_tool), unit tests
catalog/                data: games, backends, recomps
platform/               per-OS packaging (Info.plist, rc, manifest, desktop file, icons)
```

## Choosing how a game runs

Each game in `catalog/games.json` lists backend options with the operating
systems they apply to and a priority. At runtime Marathon picks, in order:

1. the backend the user explicitly selected on the game page;
2. the highest-priority option that is already installed or linked;
3. the highest-priority option that can be installed automatically;
4. anything else that is available on this OS.

Native recompilations and source ports have the highest priority; emulators
are the fallback. On Android and iOS external programs cannot be started, so
games run through libretro cores inside Marathon, or (Android only) are handed
to another installed emulator app with an Intent.

## Backend kinds

| kind | Runs as | Installed by |
|---|---|---|
| `recomp`, `port` | external process (desktop) | GitHub release download, source build, or "link existing" |
| `external` | external process (desktop) | official download or "link existing" |
| `libretro` | inside Marathon (all OS) | buildbot download (desktop, Android) or bundled (iOS) |
| `android-app` | another Android app | the user installs it from its store |

Every adapter translates the unified `GameSettings` into the backend's own
configuration and advertises what it honours through `capabilities`. The UI
disables settings a backend can't apply, so nothing silently does nothing.

## Threads

The UI runs on the main thread. Scans, hashing and installs run as `Job`s on
worker threads with a shared `TaskProgress`. Installs write into a scratch copy
of the library; the main thread merges the result when the job ends.

## Legal boundaries

- Marathon contains no game data and never downloads any.
- Backends without a redistribution licence (e.g. Rage Racer PC) are only ever
  downloaded by the user's own machine from the upstream release page.
- Recomps that generate code from the disc (psxrecomp titles) build locally,
  from the user's disc, through their own setup tools.
