# Adding to the catalog

The catalog that ships with Marathon (`catalog/games.json`, `backends.json` and
`recomps.json`) says which games Marathon knows and which backends can run them. You
can add to it without rebuilding Marathon, for example to run your own build of a port
or emulator next to the official one: put extra catalog files in the `catalog` folder
inside Marathon's data folder.

| OS | Folder for extra catalog files |
|---|---|
| macOS | `~/Library/Application Support/RidgeRacerMarathon/Marathon/catalog/` |
| Windows | `%APPDATA%\RidgeRacerMarathon\Marathon\catalog\` |
| Linux | `~/.local/share/RidgeRacerMarathon/Marathon/catalog/` |

If you start Marathon with `--data <folder>` (or `RRM_DATA_DIR`), it is the `catalog`
folder inside that one. Marathon reads every `*.json` file there at start-up, in file
name order, after its own catalog. Restart Marathon after changing them.

## Format

```json
{
  "schema": 1,
  "backends": [
    {
      "id": "pcsx2-own",
      "adapter": "pcsx2",
      "name": "PCSX2 (my build)",
      "kind": "external",
      "capabilities": { "resolution": ["native", "2x", "4x", "4k"], "aspect": ["4:3", "16:9"] },
      "sources": {
        "macos-arm64": {
          "kind": "user",
          "executable": "PCSX2*.app/Contents/MacOS/PCSX2",
          "note": "Choose your own PCSX2 build with Link existing."
        }
      }
    }
  ],
  "gameOptions": {
    "ridge-racer-v": [
      { "backend": "pcsx2-own", "os": ["macos"], "priority": 90, "status": "experimental",
        "note": "My own PCSX2 build." }
    ]
  }
}
```

- `backends` takes the same entries as `catalog/recomps.json` and `catalog/backends.json`.
  An entry whose `id` already exists replaces that backend completely; a new `id` adds
  one. The easiest start is to copy a whole entry from those files and change it.
- `adapter` says which of Marathon's adapters runs the backend (the ids in
  `catalog/backends.json` and `catalog/recomps.json`, such as `pcsx2` or `ppsspp`).
  It defaults to the `id`, so a new `id` needs it. The adapter reads the entry's own
  `params`; see its source in `src/backends/` for the ones it understands. Libretro cores (`"kind": "libretro"`) and Android apps are chosen by
  `kind` instead.
- `gameOptions` maps a game id from `catalog/games.json` to options in the same form as
  that game's `backends` list. They are added to the game's options; the highest
  `priority` that is installed is used unless you pick one on the game page.
- A source with `"kind": "user"` is never downloaded: you choose the program with
  **Link existing**. Its optional `marker` names a file that must exist next to the
  program (relative to the program's folder), so Marathon accepts only a build that has
  it, not another one with the same program name.

## When something is wrong

If a file cannot be read, is not valid JSON, has an unsupported `schema`, names a game or
backend that does not exist, or has an entry of the wrong shape, Marathon skips that
whole file, logs a warning (`[catalog] skipped <file>: <reason>`) and starts normally
with the rest. Removing a file hides its options again; a build you linked stays
remembered and comes back when the file does.

## Trying an addition without the app

The developer tools `launch_tool` and `recomp_tool` read the extra files in the
`catalog` folder of their `--data` folder too. For example

```sh
mkdir -p /tmp/test-data/catalog && cp my-addition.json /tmp/test-data/catalog/
build/launch_tool matrix ~/Games --data /tmp/test-data --only ridge-racer-v
```

lists every option for the game, including yours, with the command line each would run.
