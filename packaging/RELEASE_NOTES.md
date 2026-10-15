Download **one runtime package for your operating system** below, extract it, and run Marathon. The emulator backends are included and install automatically on first launch, without separate emulator downloads. `START_HERE.txt` is inside each package.

- **macOS Apple Silicon:** [Download RidgeRacerMarathon-macos-arm64.zip](https://github.com/channelramble/RIDGE-RACER-MARATHON/releases/download/v0.2.0/RidgeRacerMarathon-macos-arm64.zip)
- **Windows x64:** [Download RidgeRacerMarathon-windows-x64.zip](https://github.com/channelramble/RIDGE-RACER-MARATHON/releases/download/v0.2.0/RidgeRacerMarathon-windows-x64.zip)
- **Linux x64:** [Download RidgeRacerMarathon-linux-x64.tar.gz](https://github.com/channelramble/RIDGE-RACER-MARATHON/releases/download/v0.2.0/RidgeRacerMarathon-linux-x64.tar.gz)

The Complete packages include SwanStation, PCSX-ReARMed, PPSSPP and MAME cores, standalone PPSSPP, PCSX2 and RPCS3; Windows also includes standalone MAME and Xenia Canary, and Linux includes Xenia Canary. Every payload is pinned and checked against its SHA-256 hash. Existing backend installations, settings and saves are preserved.

Controller support includes gamepad navigation throughout Marathon, a live controller test, steering/pedal/dead-zone/player settings, and configurations for the supported emulator and native-port backends. Hold Guide + Start to return to Marathon; Back + Start is available where the operating system reserves Guide.

On macOS, holding a gameplay or navigation key no longer opens the accent-character picker. Text entry is enabled only while an editable field has focus.

**Bring your own game files.** No game images, extracted game code/assets, BIOS or console firmware are included. PCSX2 needs your PS2 BIOS and RPCS3 needs official PS3 system software. Native ports/recompilations that are distributed only by upstream or generated from your disc remain available through **Backends > Install all** or **Link existing**. The package contains emulator alternatives where supported; there is no Xbox 360 emulator for macOS.

The `rrm-source-*` files are matching emulator/dependency source archives and are **not needed to play**. `source-manifest.json` records their provenance; `SOURCE-README.txt` explains extraction, including any split archives. `SHA256SUMS.txt` covers every release download. See [bundle details](https://github.com/channelramble/RIDGE-RACER-MARATHON/blob/v0.2.0/docs/BUNDLED_BACKENDS.md).

Validation includes controller tests, hostile/corrupt bundle fixtures, app startup on all three operating systems, first-run offline import of every included backend and loading each libretro core. This does not establish gameplay compatibility for every game/controller combination. High-refresh frame generation and RR7 ultrawide retain the limitations documented in the README.

The macOS app is ad-hoc signed, not notarized: after the first blocked launch, use System Settings > Privacy & Security > Open Anyway. The Windows executable is unsigned: SmartScreen may require More info > Run anyway. Marathon supports macOS 11+ and Linux glibc 2.35+, but individual external backends can require newer operating systems, drivers or the Visual C++ x64 runtime.
