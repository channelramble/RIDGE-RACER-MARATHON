# Contributing

Thanks for helping. Bug reports, catalog updates (new upstream releases, renamed
release files, new dump hashes), adapter fixes and tests on Windows and Linux are
all welcome. For larger changes, open an issue first so the approach can be agreed.

## Ground rules

- **No game data, ever.** Never commit game files, disc images, ROMs, BIOS files,
  console firmware (such as `PS3UPDAT.PUP`), decrypted executables, memory cards,
  save states, or anything extracted or generated from them (textures, audio, code
  produced by a recompiler from a disc). This also applies to test fixtures: the
  scanner tests build synthetic media instead. Hashes, serials and file names are fine.
- **Only code you may share.** Third-party code needs a licence that allows it, and it
  is listed in `THIRD_PARTY_NOTICES.md` with its full text in `LICENSES/`. Projects that
  publish no licence are only ever downloaded by the user's machine from their official
  release page; don't copy their code, patches or binaries into this repository.
- **Adapters never write into a user's own installs.** An install the user linked, their
  own emulator setup, their game folders and their disc images are not Marathon's to
  change. Settings reach a backend through command-line flags, environment variables or
  a configuration folder inside Marathon's data folder; saves, caches and firmware go
  there too. Where a backend takes some settings only from its own file (a PSXRecomp
  build's `settings.toml` beside it, Fukami's `fukami.ini`), change only the keys Marathon
  owns with the comment-preserving editors in `src/core/ConfigFiles.*`, and say so in the
  adapter's header comment.
- **Tests never touch a real install.** The test tools take `--data <folder>` (default: a
  fresh temp folder). Test against copies of your own builds and games, never the originals.

## Building and testing

[docs/BUILDING.md](docs/BUILDING.md) has the full instructions. The checks CI runs on
macOS, Linux and Windows (`.github/workflows/build.yml`) are:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRRM_BUILD_TESTS=ON -DRRM_VENDORED_SDL=ON
cmake --build build

# scanner unit tests (a separate project; here it uses the SDL built above)
SDL3_DIR="$PWD/build/_deps/sdl3-build" cmake -S tests/scanner -B build-scanner -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-scanner
ctest --test-dir build-scanner --output-on-failure

# catalog smoke test: launch_tool matrix on an empty folder must list every game
cmake -DLAUNCH_TOOL=build/launch_tool -DCATALOG=catalog -DWORK=build/catalog-smoke \
      -P .github/scripts/catalog-smoke.cmake
```

With Visual Studio (MSVC), leave out `-G Ninja` and `-DCMAKE_BUILD_TYPE`, add
`--config Release` to the builds and `-C Release` to `ctest`, and use
`build/Release/launch_tool`.

When you change an adapter or the catalog, also run `launch_tool matrix <your games
folder>` with your own dumps (a dry run: it prints the command lines and core options
Marathon would use, and starts nothing), and play the affected game once if you can.
Say in the pull request what you tested and on which OS and hardware.

## Code style

- C++20 with SDL3 and Dear ImGui (`tools/framegrab` is Objective-C++ / C++17). Match the
  code around your change: 4-space indentation, braces on the same line, `camelCase`
  functions, `PascalCase` types, `kConstants`, `g_globals`, code under `src/` in
  namespace `rrm`, lines up to about 120 columns.
- Comments explain why, not what. Every adapter in `src/backends/` starts with a comment
  block that documents which config keys, flags, patches and cheats each setting
  becomes; keep it in step with the code.
- Keep changes focused, and update the docs (`README.md`, `docs/`) and the catalog
  notes when behaviour changes. Documentation is plain language: say what was measured
  or tested and what was not.
- Catalog files are data: see [docs/CATALOG.md](docs/CATALOG.md) for the format.

## Reporting problems

Use the bug report form. It asks for the OS, the Marathon version (top of the About
page), the backend and the backend log: Marathon writes one log per backend to the
`logs` folder inside its data folder (Settings > Open data folder, or the Log button
while a game runs).

Catalog and backend problems are common when an upstream project publishes a new
release, so please include:

- the backend and its version, and whether Marathon installed it or you linked your
  own build;
- for downloads: the error from the log and the name of the file on the upstream
  release page;
- for dumps that aren't recognised or don't verify: the format (CUE/BIN, ISO, CHD,
  folder, zip), region and serial, and the verification result on the game page.

Never attach or link game files, ROMs, BIOS, firmware or decrypted executables, not
even parts of them.

## Licence

By contributing you agree that your contribution is released under the project's MIT
licence ([LICENSE](LICENSE)).
