#!/usr/bin/env bash
# Cross-compiles Ridge Racer Marathon for Windows x64 with mingw-w64 and packages
#   dist/RidgeRacerMarathon-windows-x64.zip
#     RidgeRacerMarathon/RidgeRacerMarathon.exe, catalog/, assets/, LICENSE, THIRD_PARTY_NOTICES.md, LICENSES/
#
# Requirements: cmake >= 3.24, ninja (optional), zip, and mingw-w64
#   macOS:  brew install mingw-w64 ninja
#   Ubuntu: sudo apt install mingw-w64 ninja-build zip
# Native MSVC builds use plain CMake instead (see docs/BUILDING.md and .github/workflows/build.yml).
source "$(dirname "$0")/common.sh"

BUILD_DIR="${BUILD_DIR:-$RRM_ROOT/build-dist/windows-x64}"
ARTIFACT="$DIST_DIR/RidgeRacerMarathon-windows-x64.zip"
TRIPLE=x86_64-w64-mingw32

command -v "$TRIPLE-g++" >/dev/null 2>&1 || command -v "$TRIPLE-g++-posix" >/dev/null 2>&1 \
    || die "mingw-w64 not found (brew install mingw-w64 / apt install mingw-w64)"
command -v zip >/dev/null 2>&1 || die "zip not found"

rrm_common_args
log "Configuring ($BUILD_DIR)"
cmake -S "$RRM_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$RRM_ROOT/cmake/toolchains/mingw-w64-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DRRM_VENDORED_SDL=ON \
    -DRRM_BUILD_TESTS=OFF \
    ${COMMON_ARGS[@]+"${COMMON_ARGS[@]}"}

log "Building"
cmake --build "$BUILD_DIR" --target RidgeRacerMarathon --parallel "$JOBS"

EXE="$BUILD_DIR/RidgeRacerMarathon.exe"
[[ -f "$EXE" ]] || die "build did not produce $EXE"

STAGE="$BUILD_DIR/package"
APPDIR="$STAGE/RidgeRacerMarathon"
rm -rf "$STAGE"
mkdir -p "$APPDIR"
cp "$EXE" "$APPDIR/"
STRIP="$(command -v "$TRIPLE-strip" || true)"
[[ -n "$STRIP" ]] && "$STRIP" --strip-unneeded "$APPDIR/RidgeRacerMarathon.exe"
cp -R "$RRM_ROOT/catalog" "$RRM_ROOT/assets" "$APPDIR/"
rrm_copy_legal "$APPDIR"
rrm_copy_bundle "$APPDIR"
if [[ -f "$APPDIR/bundle/prerequisites/Install-Windows-Runtime.ps1" ]]; then
    cp "$RRM_ROOT/packaging/Install Windows Runtime.cmd" "$APPDIR/Install Windows Runtime.cmd"
fi
rrm_check_sdl_licenses "$BUILD_DIR"
find "$APPDIR" -name .DS_Store -delete

# The exe must only import system DLLs (no libstdc++/libgcc/winpthread/SDL3.dll).
OBJDUMP="$(command -v "$TRIPLE-objdump" || true)"
if [[ -n "$OBJDUMP" ]]; then
    IMPORTS="$("$OBJDUMP" -p "$APPDIR/RidgeRacerMarathon.exe" | sed -n 's/.*DLL Name: //p' | sort -u)"
    log "Imported DLLs: $(echo "$IMPORTS" | tr '\n' ' ')"
    if echo "$IMPORTS" | grep -Eiq 'libstdc|libgcc|winpthread|SDL3'; then
        die "unexpected non-system DLL dependency"
    fi
fi

rm -f "$ARTIFACT"
(cd "$STAGE" && zip -qr -9 -X "$ARTIFACT" RidgeRacerMarathon)
log "Wrote $ARTIFACT ($(human_size "$ARTIFACT"))"
