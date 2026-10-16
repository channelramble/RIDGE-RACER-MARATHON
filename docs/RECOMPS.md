# Recompilations, ports and emulators

Marathon prefers a native recompilation or source port when one exists and falls back
to an emulator. Complete desktop packages include the redistributable backends and
install them automatically, offline. See [the platform inventory](BUNDLED_BACKENDS.md)
for included backends and the remaining upstream-download or source-build options.

| Game | Project | Licence | How Marathon gets it |
|---|---|---|---|
| Ridge Racer (PS1) | [RobGreenUK/Ridge-Racer-PS1-Recomp](https://github.com/RobGreenUK/Ridge-Racer-PS1-Recomp) | MIT + PolyForm Noncommercial (PSXRecomp) | source only: build it with your disc, then Link existing |
| R4 (PS1) | [tetrisgm/RidgeRacerType4Recomp](https://github.com/tetrisgm/RidgeRacerType4Recomp) (macOS, Windows); [omegakatana92/RidgeRacer4Recomp](https://github.com/omegakatana92/RidgeRacer4Recomp) (Linux) | MIT + PolyForm Noncommercial; Linux build unlicensed | included setup kit on macOS/Windows (first start compiles from your disc); upstream download on Linux |
| Rage Racer (PS1) | [khasinski/rage-racer-pc](https://github.com/khasinski/rage-racer-pc) | none published | release download |
| Rave Racer (arcade) | [spacestate1/namco22-decompile](https://github.com/spacestate1/namco22-decompile) | MIT (generated code derives from the ROMs) | included on all desktop platforms; Apple Silicon needs macOS 14+ |
| Ridge Racer V (PS2) | [danisandoval/fukami](https://github.com/danisandoval/fukami) | GPL-3.0 | included on all desktop platforms |
| Ridge Racer 6 (X360) | [Sirhalo23/RidgeRacer6-Recompilation](https://github.com/Sirhalo23/RidgeRacer6-Recompilation) | BSD-3-Clause | included on all desktop platforms; Apple Silicon needs macOS 14+ |
| PS1 / PSP / arcade | SwanStation, PCSX-ReARMed, PPSSPP, MAME libretro cores | GPL | included on desktop/iOS; libretro buildbot on Android |
| PS2 / PS3 / X360 / PSP / arcade | PCSX2, RPCS3, Xenia Canary, PPSSPP, MAME | GPL / BSD | included where supported; see the platform inventory |

## Marathon's own additions

Where upstream stops short of ultrawide or high frame rates, Marathon adds what it safely can:

- **Ridge Racer:** the native-scene renderer projects from the image height, so
  Marathon opens it at 21:9, 32:9 or your screen's shape directly. The recomp's own
  service menu only offers 4:3 and 16:9 and is never given the wider sizes.
- **R4:** passes 32:9 through to the recomp's widescreen mod, which supports it.
- **Ridge Racer V on PCSX2:** a 21:9 / 32:9 / screen-shaped patch derived from PCSX2's
  16:9 patch. All of its values follow one Hor+ rule, so they scale to any ratio.
  Experimental.
- **Ridge Racer / Ridge Racer 2 on PSP:** writes the European discs' CWCheat
  widescreen code with the exact ratio, in-app and in standalone PPSSPP.
- **Ridge Racer 6 on Xenia:** sets the community aspect-ratio patch to the exact ratio.
  Its module hash matches the USA disc too.
- **Ridge Racer 7 on RPCS3:** Marathon's own patch for the USA disc (BLUS30001) widens
  the race view (Hor+) in full screen on a screen wider than 16:9; the 2D HUD is stretched.
- **In-app cores:** optional motion-interpolated (or blended) in-between images above
  the game's frame rate.
- **Ridge Racer 6 and 7 on macOS:** frame generation for the RR6 recomp and RPCS3
  (see [SETTINGS.md](SETTINGS.md)).

## Rave Racer on macOS

The Complete package includes a native Apple Silicon runtime for macOS 14 or newer.
Marathon installs it automatically and prepares its ROM sets from your own files.
No Homebrew, compiler, separate SDL installation or manual linking is needed.
Backends > Install can also download the same checksum-verified runtime.

Rebuilding is optional. Developers can use `scripts/build-rave-macos.sh` with their
own `raverace.zip` and `namcoc74.zip`; see [the build instructions](BUNDLED_BACKENDS.md#reproducible-release-inputs).
The Mac portability patch and matching sources are included with the release.
Game ROMs and extracted game assets are not distributed.
