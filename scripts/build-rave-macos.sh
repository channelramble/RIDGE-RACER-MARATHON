#!/usr/bin/env bash
# MIT; Copyright (c) 2026 Ridge Racer Marathon contributors.
# Build a redistributable runtime. The two user-supplied ROM archives stay private.
set -euo pipefail
if [[ $# != 3 ]]; then
  echo "Usage: $0 /absolute/raverace.zip /absolute/namcoc74.zip /absolute/output" >&2
  exit 2
fi
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
ROM=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
C74=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
mkdir -p "$3"
OUT=$(cd "$3" && pwd)
WORK=${RAVE_BUILD_DIR:-"$ROOT/local/rave-release-build"}
GCC=${RAVE_GCC:-gcc-16}
JOBS=${RAVE_JOBS:-6}
REV=a9dab183eb29e941629b473e4172b16835576d94
SDL_REV=5d249570393f7a37e037abf22cd6012a4cc56a71
[[ $(uname -sm) == 'Darwin arm64' ]] || { echo 'Requires an Apple Silicon Mac.' >&2; exit 1; }
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
checkout() {
  local repo=$1 rev=$2 dest=$3
  if [[ ! -d "$dest/.git" ]]; then
    git init "$dest"
    git -C "$dest" remote add origin "https://github.com/$repo.git"
    git -C "$dest" fetch --depth=1 origin "$rev"
    git -C "$dest" checkout --detach FETCH_HEAD
  fi
  [[ $(git -C "$dest" rev-parse HEAD) == "$rev" ]]
}
checkout spacestate1/namco22-decompile "$REV" "$WORK/namco22"
checkout libsdl-org/SDL "$SDL_REV" "$WORK/sdl2"
PATCH="$ROOT/packaging/source-patches/rave-macos.patch"
[[ -f "$PATCH" ]] || PATCH="$SCRIPT_DIR/rave-macos.patch"
if git -C "$WORK/namco22" apply --check "$PATCH"; then
  git -C "$WORK/namco22" apply "$PATCH"
else
  git -C "$WORK/namco22" apply --reverse --check "$PATCH"
fi
python3 "$WORK/namco22/raverace/tools/setup_roms.py" "$ROM" "$C74"
cmake -S "$WORK/sdl2" -B "$WORK/sdl2/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
  -DCMAKE_INSTALL_PREFIX="$WORK/sdl2/install"
cmake --build "$WORK/sdl2/build" --parallel "$JOBS"
cmake --install "$WORK/sdl2/build"
cmake -S "$WORK/namco22/raverace" -B "$WORK/namco22/raverace/build" -G Ninja \
  -DCMAKE_C_COMPILER="$(command -v "$GCC")" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DSDL2_DIR="$WORK/sdl2/install/lib/cmake/SDL2"
cmake --build "$WORK/namco22/raverace/build" --target rr --parallel "$JOBS"
PKG="$OUT/RaveRacer-macos-arm64"
mkdir -p "$PKG/licenses"
cp "$WORK/namco22/raverace/build/rr" "$PKG/rr"
strip -x "$PKG/rr"
codesign --force --sign - "$PKG/rr"
codesign --verify --strict "$PKG/rr"
python3 - "$PKG/rr" <<'PY'
import subprocess, sys
exe = sys.argv[1]
assert subprocess.check_output(['lipo', '-archs', exe], text=True).strip() == 'arm64'
lines = subprocess.check_output(['otool', '-L', exe], text=True).splitlines()[1:]
for line in lines:
    dependency = line.strip().split(' (', 1)[0]
    assert dependency.startswith(('/usr/lib/', '/System/Library/')), dependency
PY
cp "$WORK/namco22/LICENSE" "$PKG/licenses/namco22-MIT.txt"
cp "$WORK/sdl2/LICENSE.txt" "$PKG/licenses/SDL2-zlib.txt"
MARATHON_LICENSE="$ROOT/LICENSE"
[[ -f "$MARATHON_LICENSE" ]] || MARATHON_LICENSE="$SCRIPT_DIR/licenses/Marathon-MIT.txt"
cp "$MARATHON_LICENSE" "$PKG/licenses/Marathon-MIT.txt"
GCC_PREFIX=$(python3 - "$(command -v "$GCC")" <<'PY'
from pathlib import Path
import sys
print(Path(sys.argv[1]).resolve().parent.parent)
PY
)
cp "$GCC_PREFIX/COPYING" "$PKG/licenses/GCC-GPL.txt"
cp "$GCC_PREFIX/COPYING.RUNTIME" "$PKG/licenses/GCC-runtime-exception.txt"
cp "$PATCH" "$PKG/rave-macos.patch"
cp "$0" "$PKG/build-rave-macos.sh"
cat > "$PKG/BUILD.txt" <<EOF
Rave Racer 0.9.0 for macOS 14+ on Apple Silicon
Namco 2x Systems: $REV
SDL2 (statically linked): $SDL_REV
Compiler: $($GCC --version | head -1)
Mac portability patch and build recipe are included. Source archives are on the
matching Ridge Racer Marathon release. ROMs and generated game source are omitted.
Run through Marathon, which prepares roms/raverace.zip and roms/namcoc74.zip from
your own files. No Homebrew, Rosetta, compiler or separate SDL install is needed.
EOF
ditto -c -k --norsrc --keepParent "$PKG" "$OUT/RaveRacer-macos-arm64.zip"
shasum -a 256 "$OUT/RaveRacer-macos-arm64.zip"
