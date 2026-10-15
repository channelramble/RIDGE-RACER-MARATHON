#!/usr/bin/env bash
# App smoke test: starts Marathon with a fresh data folder, lets it draw its library screen for
# 120 frames, saves a screenshot and exits. Fails unless the app exits 0 and the PNG is written.
#
#   .github/scripts/smoke-app.sh <Marathon executable> <output dir>
#
# CI machines have no GPU driver with OpenGL 3.3, so the app draws with Mesa's software renderer:
# on Linux under a virtual X display (xvfb), on Windows with Mesa's opengl32.dll placed beside
# the exe (a copy of the exe's folder is used, the package itself stays as it is). macOS runners
# provide OpenGL themselves.
set -euo pipefail

exe="$1"
out="$2"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
data="$out/appdata"
png="$out/library.png"
rm -rf "$data" "$png"
[[ -f "$exe" ]] || { echo "smoke-app: $exe not found" >&2; exit 1; }

run=("$exe")
case "$(uname -s)" in
    Linux)
        if ! command -v xvfb-run >/dev/null 2>&1; then
            sudo apt-get install -y -q --no-install-recommends xvfb libgl1-mesa-dri >/dev/null
        fi
        export LIBGL_ALWAYS_SOFTWARE=1
        run=(xvfb-run -a -s "-screen 0 1280x800x24" "$exe")
        ;;
    MINGW* | MSYS* | CYGWIN*)
        mesa_version=26.2.4
        mesa="$out/mesa"
        if [[ ! -f "$mesa/x64/opengl32.dll" ]]; then
            curl -fsSL -o "$out/mesa.7z" \
                "https://github.com/pal1000/mesa-dist-win/releases/download/$mesa_version/mesa3d-$mesa_version-release-msvc.7z"
            7z x -y -o"$mesa" "$out/mesa.7z" >/dev/null
        fi
        # Run from a copy of the exe's folder, with Mesa's DLLs beside the exe.
        app="$out/app"
        rm -rf "$app"
        cp -R "$(dirname "$exe")" "$app"
        cp "$mesa"/x64/*.dll "$app"/
        export GALLIUM_DRIVER=llvmpipe
        run=("$app/$(basename "$exe")")
        ;;
esac

echo "smoke-app: ${run[*]}"
status=0
"${run[@]}" --data "$data" --wait-for-setup --frames 120 --screenshot "$png" || status=$?
echo "smoke-app: exit status $status"
[[ $status -eq 0 ]] || exit "$status"
[[ -s "$png" ]] || { echo "smoke-app: no screenshot at $png" >&2; exit 1; }
echo "smoke-app: screenshot $png ($(wc -c <"$png") bytes)"
