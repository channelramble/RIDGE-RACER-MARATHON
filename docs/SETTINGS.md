# Enhancement settings

Every game page has the same settings. Each backend translates them into its own
configuration and declares what it can honour (`capabilities` in the catalog);
the UI disables what a backend can't do instead of silently ignoring it.

| Setting | Values | Meaning |
|---|---|---|
| Resolution | native, 2x-10x, 720p-8k, display | Internal rendering height. Backends round to the steps they support. |
| Aspect ratio | auto, 4:3, 16:9, 21:9, 32:9, Match my screen | Wider than 4:3 shows more of the scene where the backend can (see below). "Match my screen" uses the display's own shape, e.g. 43:18 on a 3440x1440 monitor. |
| Frame rate | original, match display, 60-360, custom | Presentation rate. Above the game's own rate the extra images are real frames (native redraws), motion-interpolated in-between images (in-app cores; on macOS also Ridge Racer 6 and 7 through frame generation) or a crossfade; game speed and physics never change. |
| Smoothing method | interpolate, blend | Interpolate: in-between images built from the motion between two game frames. Blend: a crossfade between game frames (some ghosting, no extra motion detail). |
| Game speed / fast-forward | 50-300 %, hold-to-fast-forward | Where the backend supports it. |

## 4K, ultrawide and high refresh, per game

"Native" ultrawide means the game really draws more of the world at the sides with the
HUD placed correctly; "hack" means a widened field of view with the 2D HUD stretched.

| Game | Best way to run it | 4K+ | Ultrawide | Above 60 Hz |
|---|---|---|---|---|
| Ridge Racer (PS1) | RR1 recomp (your local build) with the native redraw | yes | **native** 21:9, 32:9 or screen shape with the redraw on; the original renderer stops at 16:9 | redraw draws real frames up to 360 fps or the display rate |
| Rave Racer (arcade) | namco22 native port (Windows/Linux; macOS local build) | up to 8K | **native**, the screen's own shape in fullscreen | the game runs at 59.9 Hz; higher rates repeat frames |
| Rage Racer (PS1) | Rage Racer PC | yes (scale 9 = 2160 lines) | the port stops at 16:9 (4:3 on 4:3-ish screens) and runs in a window; 21:9 / 32:9 via SwanStation (hack) | interpolated up to 1000 fps |
| R4 (PS1) | R4 recomp | up to 8K | **native** Fit, 21:9, 32:9 (races; menus stay 4:3) | interpolated to the display rate |
| Ridge Racer V (PS2) | Fukami (all desktops); PCSX2 | up to 3584 lines | Fukami **native** 21:9 (bars beyond); PCSX2: 16:9 patch, plus 21:9 / 32:9 / screen shape derived from it (experimental, HUD stretched) | 60 fps; VRR or a 120/240 Hz display mode keeps it even |
| Ridge Racer (PSP), Ridge Racer 2 (PSP) | PPSSPP (in-app core or standalone) | up to 10x | European discs only: any ratio through a widescreen code (hack) | 60 fps; in-app frame blending |
| Ridge Racer 6 (X360) | RR6 recomp (Windows/Linux; macOS local build); Xenia | up to 4x (2880 lines) | recomp **native** 21:9, 32:9, screen shape in fullscreen with the HUD kept in shape; Xenia: aspect patch (hack) | game speed is tied to 60 Hz; macOS: frame generation up to 360 fps (measured on an M4: menus at 60 fps, races at 28-58 fps at 720p, so slower than full speed) |
| Ridge Racer 7 (PS3) | RPCS3 | Resolution Scale up to 8K | Marathon's own patch (USA disc BLUS30001): in full screen on a screen wider than 16:9, at the screen's shape; Hor+ race view, the 16:9 picture stretched to the screen, 2D HUD stretched | 60 fps; macOS: frame generation up to 360 fps |

Every PS1 game also runs in-app through SwanStation with ultrawide (hack, any ratio)
and up to 16x resolution, on every platform including phones.

## In-app cores

SwanStation, PCSX-ReARMed, PPSSPP and MAME run inside Marathon's own window. When the
display is faster than the game (60 fps content on a 120/144/240 Hz screen) and the
frame rate is set above "original", Marathon shows in-between images instead of
repeating each game frame: with "interpolate" it estimates the motion between two game
frames (block matching on a small image pyramid) and moves the picture along it, with
"blend" it crossfades. Either way the game itself runs at its own speed and the newest
frame is shown about one game frame late. A frame-rate cap below twice the game's rate
turns it off. Measured on real game frames (the middle of three frames rebuilt from the
outer two, PSNR against the real one): Ridge Racer 2 29.4 dB interpolated against 22.9
blended, Rave Racer 27.5 against 24.9.

## Frame generation (macOS: Ridge Racer 6 and 7)

The RR6 recomp and RPCS3 draw at 60 fps. On a faster display, Marathon starts them with
its present library (`librrm_present.dylib`, from `tools/framegrab`), which shows
motion-interpolated in-between images up to the chosen rate (360 fps at most, and no
more than the display of the game window can show; 120 Hz shows 2 images per game frame,
240 Hz 4, 360 Hz 6). The game keeps presenting its own frames exactly as before; the
library copies each one after it is drawn, estimates the motion on the GPU (on an M4 about
1.2 ms per game frame at 1080p and 1.7 ms at 4K, plus 0.2 / 0.6 ms per extra image) and
shows its images on a layer of its own over the game's. Cost: one game frame of extra latency. It does not
apply to programs signed with the hardened runtime (macOS ignores the library there) or
to Fukami, which starts the game with a fresh environment. On a fixed-rate display that
is not a whole multiple of 60 Hz (144, 165), each image is held for whole refreshes, so
motion is not perfectly even.

## Per-backend details

The adapters in `src/backends/` document exactly which config keys, command-line flags,
patches and cheats each setting becomes (one comment block per file).
