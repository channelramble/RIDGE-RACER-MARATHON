# Controllers

Marathon works with a gamepad from start to finish: its own menus, every game it starts
and closing a game again. Xbox, PlayStation, Switch Pro and most other controllers SDL
knows work over USB or Bluetooth; Marathon shows the right button names for each of
them. A keyboard and mouse keep working everywhere as before.

- [1. Using Marathon with a controller](#1-using-marathon-with-a-controller)
- [2. The controller settings](#2-the-controller-settings)
- [3. Players](#3-players)
- [4. Closing a game from the controller](#4-closing-a-game-from-the-controller)
- [5. Per game and backend](#5-per-game-and-backend)
- [6. What Marathon writes for each backend](#6-what-marathon-writes-for-each-backend)
- [7. Advanced: per-game overrides and core options](#7-advanced-per-game-overrides-and-core-options)
- [8. Troubleshooting](#8-troubleshooting)
- [9. Not checked with a real controller yet](#9-not-checked-with-a-real-controller-yet)

Button names below are the Xbox ones, with the PlayStation names in brackets where they
differ. Marathon itself always shows the names printed on your controller.

## 1. Using Marathon with a controller

| Button | In Marathon's menus |
|---|---|
| D-pad | move between buttons, cards and settings |
| Left stick | scroll the page |
| A (Cross) | select, tick, open a list; on a slider, A starts and ends changing it |
| B (Circle) | back: closes a list, leaves the game page or the Controllers page |
| LB / RB (L1 / R1) | previous / next tab: Library, Add games, Backends, Settings, About |
| LT / RT (L2 / R2) | on Settings: switch between General and Controllers |
| Start (Options) | on the library: race the game under the cursor if it is ready, otherwise open its page; on a game page: race |
| Y (Triangle) | open the settings of the game under the cursor |
| Guide + Start, held | close the running game (section 4) |

While a controller is connected, a bar at the bottom of the window shows the buttons
that do something on the current page, drawn the way your controller labels them. When
you reach a page with the controller, the first useful item (the game you came from, or
RACE! on a game page) is already selected and outlined in yellow. A "(?)" next to a
setting explains it; with a controller the explanation appears as soon as the setting is
selected.

**Nintendo controllers:** the button labelled A selects and the one labelled B goes back,
as on the console, even though they sit on the other side than on Xbox and PlayStation
controllers. The same goes for Y (game settings), which is the left button on a Switch Pro.

Marathon's menus follow the controller only while Marathon's window is in front and no
game is running in another program; inside a game the controller belongs to the game.

## 2. The controller settings

*Settings > Controllers* (LT / RT on the Settings tab). These apply to every game; each
backend uses what it can (section 5, and the CONTROLS panel on every game page). Changes
are saved at once and apply the next time a game starts.

| Setting | Choices | Default | What it does |
|---|---|---|---|
| Steering | Left stick (analog), D-pad (digital) | Left stick | Analog steers in proportion to the stick; the d-pad still steers where the game allows. Digital steers at full lock like the original pad; where Marathon can, the stick then works as a d-pad. |
| Accelerate and brake | Triggers (RT gas, LT brake), Buttons (the game's own layout) | Triggers | Triggers are analog where the game reads them; A and X (Cross and Square) keep working. Buttons keeps the original layout: the triggers do what the game uses them for (shifting, nitrous, views). |
| Vibration | on / off | on | Rumble in the games that have it. |
| Stick dead zone | 0-40 % | 8 % | How far the stick moves before the car steers. Raise it if the car drifts with the stick at rest. |
| Player 1, Player 2 | Automatic, Nobody (Player 2), a controller | Automatic | Section 3. |
| Close the game with | Hold Guide + Start, Hold Back + Start, Off | Guide + Start | Section 4. |
| Hold for | 0.5-3.0 s | 1.0 s | How long the buttons must be held. |

The same page lists the connected controllers (model, USB or wireless, battery), lets you
make one vibrate, and has a **Test**: it shows both sticks before and after the dead zone,
the triggers and every button as Marathon reads them. During the test the controller does
not move around Marathon; hold B (Circle) for a second, press Esc or click *Stop test* to
end it.

## 3. Players

*Automatic* gives Player 1 the first controller connected and Player 2 the second.
Choosing a controller for a player makes that model play there whenever it is connected;
the choice is recognised by the model (SDL's controller ID), so two identical controllers
follow the order they were connected in. Player 2 can also be *Nobody*.

A player keeps their controller while others come and go: if Player 1's controller is
unplugged, Player 2 stays Player 2, and the next controller that connects becomes Player 1.
Marathon shows "Controller connected: ... (Player 1)" and "Controller disconnected: ..."
when that happens. Repeated connection messages for the same model are limited to one
every two seconds, and brief repeated disconnect/reconnect cycles are suppressed.
In a game running inside Marathon, losing Player 1's
controller pauses the game until you choose Resume.

Programs that number controllers themselves (PCSX2, Xenia, Fukami) ignore this choice;
their CONTROLS panel says so.

## 4. Closing a game from the controller

Hold **Guide + Start** (the Xbox, PS or Home button together with Start) for one second
to close the running game, in every game Marathon starts:

- for a program such as an emulator or a recompilation, Marathon asks it to close, as
  its window's close button would, and stops it if it has not closed after 6 seconds;
  Marathon's window then comes back to the front;
- for a game running inside Marathon, it quits to Marathon (a ring in the corner fills
  up while you hold the buttons).

Controllers without a Guide button use Back + Start instead. You can choose Back + Start
for every controller, change the time, or turn the chord off. A short vibration confirms
it when vibration is on. On macOS the Guide button may open Launchpad or the Games app
instead of reaching Marathon; choose Back + Start then (the controller Test shows whether
Guide arrives).

Each program also has its own menu or way to quit:

| Backend | Its own menu or quit, from the pad and the keyboard |
|---|---|
| Games inside Marathon (SwanStation, PCSX-ReARMed, PPSSPP and MAME cores) | Guide or Esc: Marathon's pause menu > Quit to Marathon |
| RPCS3 (Ridge Racer 7) | Guide, or Start + Back: RPCS3's Home menu > Exit Game |
| PCSX2 (Ridge Racer V) | Back + Start (a binding Marathon adds) or Esc: the pause menu > Close Game |
| Xenia Canary (Ridge Racer 6) | none: close the window |
| PPSSPP (Ridge Racer and Ridge Racer 2, PSP) | Guide or Esc: the pause menu > Exit |
| MAME (Rave Racer) | Back + Start: MAME's menu; Back + B: quit; Tab / Esc on the keyboard |
| Ridge Racer recompilation | close the window; Esc in the native-scene redraw |
| R4 recompilation | Guide or Start + Select: R4's menus; close the window to quit |
| Rage Racer PC | Esc quits at once (keyboard); no quit from the pad |
| Rave Racer native port | Start, R3 or Esc: the game's menu (Esc twice exits) |
| Fukami (Ridge Racer V) | Guide, Esc or F1: Fukami's menu > Quit |
| Ridge Racer 6 recompilation | Back + Start held 1 s, or Esc: "Quit Ridge Racer 6?" (A confirms) |

## 5. Per game and backend

Which of the settings each backend applies:

| Backend | Steering | Accelerate and brake | Vibration | Dead zone | Players / choice |
|---|---|---|---|---|---|
| In-app SwanStation / PCSX-ReARMed (Ridge Racer, Rage Racer, R4) | yes | yes | R4 with a DualShock only | yes | 1 (R4: 2) / yes |
| In-app PPSSPP (PSP games) | yes | yes (digital) | none (the PSP has none) | yes | 1 / yes |
| In-app MAME (Rave Racer) | analog only | analog always | none | yes | 1 / yes |
| RPCS3 (Ridge Racer 7) | yes | yes | none in Ridge Racer 7 | yes | 2 / yes |
| PCSX2 (Ridge Racer V) | no | yes | yes | yes | 2 / PCSX2's order |
| Xenia Canary (Ridge Racer 6) | no | the game's own option | yes | yes | 2 / Xenia's order |
| PPSSPP standalone | yes | yes | none | yes | 1 |
| MAME standalone | analog only | analog always | none | yes | 1 |
| Ridge Racer recompilation | no (digital) | no | none | no | 1 / yes |
| R4 recompilation | yes | yes (control scheme) | yes | yes (Modern controls) | 2 / yes |
| Rage Racer PC | yes | analog always | not in 0.6.8 | yes | 1 |
| Rave Racer native port | analog | analog always | none | yes (Marathon's own install) | 1 |
| Fukami | stick only | no | yes | no | 2 / Fukami's order |
| Ridge Racer 6 recompilation | the game's own | the game's own | the game's own option | no | 1 |

The quit chord (section 4) works for every row.

The layouts with the default settings (left stick steering, trigger pedals):

**Ridge Racer, Rage Racer, R4 inside Marathon** (the game sees a neGcon, the analog
controller these games were made for):
- Left stick: steer (analog neGcon twist); the d-pad steers at full lock
- RT / LT: accelerate / brake (analog); A / X (Cross / Square): full throttle / brake
- LB / RB: neGcon L / R
- Menus in Ridge Racer: B (Circle) confirms, Y (Triangle) goes back; in Rage Racer:
  A or B (Cross or Circle) confirms, X or Y (Square or Triangle) goes back; in R4:
  A (Cross) confirms
- Start: pause

With *D-pad (digital)* steering they get the original digital pad: the d-pad or the stick
steers at full lock, A (Cross) accelerates, X (Square) brakes, and RT / LT also accelerate
and brake (with *Buttons*, LB/RB and LT/RT shift). With *Buttons*, R4 gets the DualShock
(analog stick, A / X accelerate and brake, LB/LT shift down, RB/RT shift up, vibration).

**Ridge Racer and Ridge Racer 2 (PSP)**, inside Marathon or in PPSSPP:
- Left stick or d-pad: steer
- RT or A (Cross): accelerate; LT or X (Square): brake
- LB: camera, RB: nitrous
- Y / B (Triangle / Circle): shift

The PSP has no analog pedals, so the triggers press the buttons.

**Rave Racer** (MAME, inside Marathon or standalone; the native port is the same apart
from the gear buttons):
- Left stick: steering wheel (analog); d-pad left/right: full lock
- RT / LT: gas / brake (analog); A / B: full
- D-pad up/down (standalone MAME also LB / RB): shift (manual)
- Y: view
- Back: insert coin

**Ridge Racer 7** (RPCS3):
- Left stick: steer (analog)
- RT / LT: accelerate / brake (analog, through the pressure of Cross / Square)
- RB / LB: nitrous / double nitrous (both: triple)
- Stick clicks: shift up / down (manual)
- Start: pause

Connect your controllers before starting the game: RPCS3 picks them at start.

**Ridge Racer V** (PCSX2):
- Left stick or d-pad: steer (analog)
- RT / LT: accelerate / brake (analog pressure); A / X: full
- LB / RB: shift down / up (manual)
- Y (Triangle): camera; right-stick click: rear view
- Start: pause

**Ridge Racer V** (Fukami):
- Left stick: steer (analog)
- A (Cross): accelerate, X (Square): brake
- LB / RB: shift down / up
- LT / RT are the PS2's L2 / R2 (rear view) in this game

Fukami has no button remapping yet, so the triggers cannot accelerate or brake there.

**Ridge Racer 6** (Xenia Canary): left stick steers (analog); the game's own controller
setting decides gas and brake (type A: A / X, RT nitrous). For analog accelerator and
brake, choose a configuration type with the pedals on the triggers in *Options >
Controller* (kept in your save).

**Ridge Racer 6** (recompilation): left stick steers, RT / LT accelerate / brake (analog),
B / Y nitrous, Start pauses. This is the game's own layout; Marathon changes nothing.

**R4** (recompilation, Modern controls): left stick steers (analog), RT / LT accelerate /
brake (analog), X / B (Square / Circle) shift down / up, RB camera, Y Rewind (when on).
With *D-pad* steering or *Buttons* it uses R4's classic layout. The older R4 AppImage
build has no Modern controls: A / X accelerate / brake.

**Ridge Racer** (recompilation): d-pad or left stick steers (digital: the game rejects
analog), A (Cross) accelerates, X (Square) brakes, LB/RB and LT/RT shift down / up. The
recompilation keeps its own button map, so Steering and pedals do not apply.

**Rage Racer** (Rage Racer PC): left stick steers (analog neGcon twist, d-pad full lock),
RT / LT gas / brake (analog; A / X full), LB / RB shift down / up, B handbrake, Y camera,
Start pause.

## 6. What Marathon writes for each backend

Marathon only writes into its own data folder, or adds keys to the configuration files it
already writes for a backend. It never edits a program's configuration in your own install
of it, your saves, or the button maps you made inside a program, except where noted
below (the PPSSPP mapping is replaced only when the Marathon settings change).

| Backend | What Marathon writes |
|---|---|
| Games inside Marathon | nothing on disk: the settings travel to the core as options when the game starts |
| RPCS3 | its global input configuration (Player 1 and 2 with your controllers, by name) in Marathon's RPCS3 folder, and "pause during the Home menu" in its config.yml. A portable RPCS3 you linked keeps its own controller settings. |
| PCSX2 | controller bindings for Pad 1 and Pad 2 in Marathon's PCSX2.ini (your keyboard bindings stay), the dead zone, vibration and a Back + Start pause-menu hotkey |
| Xenia Canary | controller input on, vibration and the stick dead zones in its config in Marathon's folder |
| PPSSPP | the complete button map in controls.ini (a backup of an older file is kept as controls.ini.bak), plus the stick dead zone; your later changes in PPSSPP stay until you change Marathon's settings |
| MAME | a controller file in Marathon's MAME folder (menu and quit on the pad, gears on LB / RB) and the joystick dead zone on the command line |
| Ridge Racer / R4 recompilations | which controller plays (settings.toml), R4's control scheme, rumble and stick dead zone (its controls mod) |
| Rage Racer PC | analog steering and the dead zone on the command line |
| Rave Racer native port | the stick dead zone, only in an install Marathon made |
| Fukami | analog steering and rumble in the settings it already shares with Marathon |
| Ridge Racer 6 recompilation | the path of its controller database on the command line |

## 7. Advanced: per-game overrides and core options

A game can use different steering, pedal, vibration and dead-zone settings than the rest.
There is no screen for it yet: turn on *Custom settings for this game* on its page, quit
Marathon, then open `library.json` in Marathon's data folder (*Settings > Open data
folder*) and add a `controllers` object to the game's `extra` (under `games` > the game's
ID > `settings`):

```json
"extra": {
  "controllers": { "steering": "dpad", "pedals": "buttons", "vibration": false, "deadzone": 12 }
}
```

Leave out what should follow the Controllers page. Players and the quit chord stay global,
and values Marathon does not know are ignored.

Games inside Marathon also accept core options in `extra.coreOptions`; they win over
what Marathon sets. Marathon's own input options all start with `rrm_`; the values are
strings:

| Key | Values | Default |
|---|---|---|
| `rrm_port0_device` ... `rrm_port3_device` | a device the core declares, by name or number (e.g. `negcon`) | the core's standard pad |
| `rrm_player1` ... `rrm_player4` | `""` (automatic), `none`, or a controller ID | `""` |
| `rrm_input_deadzone` | `0` ... `40` (percent) | `0` |
| `rrm_input_stick_to_dpad`, `rrm_input_dpad_to_stick` | `0` / `1` | `0` |
| `rrm_input_r2_as`, `rrm_input_l2_as` | `""`, `b`, `a`, `y`, `x`, `l`, `r`, `l3`, `r3`, `select`, `start` | `""` |
| `rrm_input_analog_triggers`, `rrm_input_left_y`, `rrm_rumble` | `0` / `1` | `1` |

## 8. Troubleshooting

- **Guide does nothing on macOS.** macOS may use the Guide / Home button itself (Launchpad
  or the Games app). Choose *Hold Back + Start* under *Closing a game*. The Test on the
  Controllers page says "Guide: not received yet" while Guide does not reach Marathon.
- **The controller does nothing in Ridge Racer 7.** RPCS3 finds controllers by name and
  picks them when the game starts: connect the controller first. If it still does
  nothing, open RPCS3's log (the Log button on the running-game banner, or `RPCS3.log`
  in Marathon's RPCS3 folder) and look at the lines starting with `SDL:` that name each
  controller; an RPCS3 built with another SDL version may name it differently.
- **Player 2 has the wrong controller in Ridge Racer V or Ridge Racer 6.** PCSX2, Xenia and
  Fukami number controllers themselves, in the order they see them: connect Player 1's
  controller first.
- **A PS4 or PS5 controller acts strangely in PPSSPP or MAME on Windows.** Without Steam
  Input those programs may see it as a generic DirectInput controller with other button
  numbers. Marathon's PPSSPP map covers the PlayStation layout; for others use Steam Input
  or the program's own control settings.
- **Something does not react.** *Settings > Controllers > Test* shows exactly what Marathon
  receives from the controller, before and after the dead zone. If the test looks right,
  the game page's CONTROLS panel tells you what that backend does with it.

## 9. Not checked with a real controller yet

Marathon's own menus, the quit chord and the in-app cores were tested with virtual
controllers; the programs Marathon starts were checked against their documented
configuration formats and, for RPCS3, by loading Marathon's configuration in a real run.
Not checked yet with real hardware:

- receiving controller input while another program has the focus, and the Guide button,
  on macOS and Windows (this is why Back + Start exists);
- Ridge Racer 7's analog throttle through Cross pressure, and RPCS3's keyboard copilot;
- R4's DualShock layout and Ridge Racer's neGcon menu buttons inside Marathon;
- PPSSPP's DirectInput lines on Windows, which fit PlayStation controllers only;
- controller names in RPCS3 builds for Windows and Linux that use another SDL version;
- whether RPCS3 and MAME close cleanly when asked; if not they simply stop at once.
