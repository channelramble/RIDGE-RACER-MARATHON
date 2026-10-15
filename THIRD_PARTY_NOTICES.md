# Third-party notices

Marathon's own code is MIT-licensed (see `LICENSE`). The components below keep
their own licences. Their full licence texts, with the copyright lines, are in
the `LICENSES/` folder (fonts: next to the font files in `assets/fonts/`), and
every package ships them:

| Package | Where the licence files are |
|---|---|
| Windows zip | `RidgeRacerMarathon/` (`LICENSE`, `THIRD_PARTY_NOTICES.md`, `LICENSES/`, `assets/fonts/`) |
| macOS zip | beside `Ridge Racer Marathon.app`, and inside it in `Contents/Resources/` |
| Linux tar.gz | the top folder, and `share/doc/ridge-racer-marathon/` |

## Compiled into Marathon

| Component | Licence | Licence text | Source |
|---|---|---|---|
| SDL 3 (`release-3.4.18`, linked statically) | zlib | `LICENSES/SDL3.txt` | https://github.com/libsdl-org/SDL |
| Dear ImGui | MIT | `LICENSES/imgui.txt` | https://github.com/ocornut/imgui |
| nlohmann/json 3.12.0 | MIT | `LICENSES/nlohmann-json.txt` | https://github.com/nlohmann/json |
| toml++ 3.4.0 | MIT | `LICENSES/tomlplusplus.txt` | https://github.com/marzer/tomlplusplus |
| miniz 11.3.2 | MIT | `LICENSES/miniz.txt` | https://github.com/richgel999/miniz |
| libretro.h (API header) | MIT | `LICENSES/libretro.txt` | https://github.com/libretro/libretro-common |
| Rajdhani font (Indian Type Foundry) | SIL OFL 1.1 | `assets/fonts/Rajdhani-OFL.txt` | https://fonts.google.com/specimen/Rajdhani |
| Titillium Web font | SIL OFL 1.1 | `assets/fonts/TitilliumWeb-OFL.txt` | https://fonts.google.com/specimen/Titillium+Web |

Code inside those components that carries its own notice:

- Dear ImGui contains stb_truetype, stb_rect_pack and stb_textedit by Sean
  Barrett (MIT or public domain, `LICENSES/stb.txt`) and an OpenGL loader
  generated with gl3w (public domain).
- SDL 3 contains:
  - HIDAPI (game controllers) by Alan Ott / Signal 11 Software, offered under
    GPL-3.0, a BSD-style licence or the original HIDAPI licence; Marathon uses
    it under the BSD-style licence: `LICENSES/SDL3-hidapi.txt`.
  - yuv2rgb by Adrien Descamps (BSD-3-Clause): `LICENSES/SDL3-yuv2rgb.txt`.
  - fdlibm math routines by Sun Microsystems: `LICENSES/SDL3-libm.txt`.
  - stb_image (MIT or public domain, `LICENSES/stb.txt`) and miniz 1.15
    (public domain) for image loading.
  - Linux builds only: X11 and Wayland driver code and the Wayland protocol
    code generated from SDL's `wayland-protocols/` files (MIT and similar):
    `LICENSES/SDL3-x11-wayland.txt`.
  - Khronos API headers (EGL, OpenGL ES, Vulkan; Apache-2.0 or MIT), used for
    type and constant declarations only.
  - Everything else in SDL, including code contributed by Valve and others,
    is under SDL's zlib licence.

The macOS app also contains `librrm_present.dylib`, which is Marathon's own
code. The developer test tools (not part of any package) also use
stb_image_write (MIT or public domain, `LICENSES/stb.txt`).

### Windows package

The Windows build is made with MinGW-w64 and links its runtime statically, so
the exe also contains:

| Component | Licence | Licence text |
|---|---|---|
| MinGW-w64 runtime | ZPL-2.1, plus the notices listed in the file | `LICENSES/mingw-w64-runtime.txt` |
| winpthreads | MIT and BSD-3-Clause | `LICENSES/winpthreads.txt` |
| GCC runtime (libstdc++, libgcc) | GPL-3.0 with the GCC Runtime Library Exception | no notice required |

## libretro cores

On desktop and Android, Marathon downloads unmodified libretro cores from the
libretro buildbot when the user installs them; it does not redistribute them.
iOS builds (not buildable yet) will have to include them, because iOS apps
cannot download executable code. Each core keeps its own licence:

| Core | Licence | Source |
|---|---|---|
| SwanStation | GPL-3.0 | https://github.com/libretro/swanstation |
| PCSX-ReARMed | GPL-2.0 | https://github.com/libretro/pcsx_rearmed |
| PPSSPP | GPL-2.0-or-later | https://github.com/hrydgard/ppsspp |
| MAME / MAME Arcade | GPL-2.0-or-later | https://github.com/libretro/mame |

Marathon's MIT-licensed code is GPL-compatible; a package that includes GPL
cores is distributed under the GPL terms of those cores.

## Downloaded on demand (desktop)

Marathon does not redistribute these programs. When the user asks, it
downloads them from their official release pages and runs them unmodified.
They are governed by their own licences:

- R4 Ridge Racer Type 4 Recomp (tetrisgm): MIT + PolyForm Noncommercial (psxrecomp)
- RidgeRacer4Recomp Linux build (omegakatana92): no licence published
- Rage Racer PC (khasinski): no licence published (all rights reserved)
- namco22-decompile / Rave Racer (spacestate1): MIT
- Fukami / Ridge Racer V (danisandoval): GPL-3.0
- Ridge Racer 6 Recompilation (Sirhalo23): BSD-3-Clause (its ReXGlue SDK
  runtime has its own licence)
- PCSX2: GPL-3.0-or-later. RPCS3: GPL-2.0. Xenia Canary: BSD-3-Clause.
- PPSSPP: GPL-2.0-or-later. MAME: GPL-2.0-or-later.
- libretro cores: SwanStation, PCSX-ReARMed, PPSSPP and MAME (licences in the
  table above).
- 7-Zip's standalone extractor 7zr.exe (Igor Pavlov): LGPL-2.1-or-later. Windows
  only, fetched from 7-zip.org to unpack .7z release archives.

## Built and linked by the user

Marathon only links these; it never downloads or redistributes them:

- Ridge Racer PS1 Recomp (RobGreenUK): MIT + PolyForm Noncommercial (psxrecomp);
  published as source only.

## Game content

No game code, BIOS, firmware, artwork or other copyrighted game data is part
of Marathon. Users must supply dumps of games and system files they own.
