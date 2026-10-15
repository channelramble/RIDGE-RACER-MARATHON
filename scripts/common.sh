# Shared helpers for scripts/build-*.sh (sourced, not executed).
#
# Environment knobs understood by every build script:
#   BUILD_DIR            CMake build directory        (default: build-dist/<platform>)
#   DIST_DIR             where artifacts are written  (default: dist/)
#   RRM_SDL_SOURCE_DIR   an existing SDL checkout to use instead of downloading SDL
#                        (passed as FETCHCONTENT_SOURCE_DIR_SDL3; must be tag release-3.4.18)
#   RRM_CMAKE_ARGS       extra arguments appended to the CMake configure command
#   JOBS                 parallel build jobs          (default: number of CPUs)
#   RRM_BUNDLE_DIR        verified offline bundle to include in this package

set -euo pipefail

RRM_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${DIST_DIR:-$RRM_ROOT/dist}"
mkdir -p "$DIST_DIR"

if [[ -z "${JOBS:-}" ]]; then
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi

log() { printf '\033[1;33m==>\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# Project version from CMakeLists.txt (project(... VERSION x.y.z ...)).
rrm_version() {
    sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$RRM_ROOT/CMakeLists.txt" | head -n1
}

# Common CMake arguments (generator, SDL source override, user extras) -> global array COMMON_ARGS.
# Expand with ${COMMON_ARGS[@]+"${COMMON_ARGS[@]}"} (safe for empty arrays on bash 3.2).
# shellcheck disable=SC2034
rrm_common_args() {
    # SDL reaches controllers through each OS's own HID API; without this it would pick up
    # whatever libusb the build machine happens to have (Homebrew's, even for a Windows cross-build).
    COMMON_ARGS=(-DSDL_HIDAPI_LIBUSB=OFF)
    if [[ -z "${CMAKE_GENERATOR:-}" ]] && command -v ninja >/dev/null 2>&1; then
        COMMON_ARGS+=(-G Ninja)
    fi
    if [[ -n "${RRM_SDL_SOURCE_DIR:-}" ]]; then
        COMMON_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_SDL3=$RRM_SDL_SOURCE_DIR")
    fi
    if [[ -n "${RRM_CMAKE_ARGS:-}" ]]; then
        # shellcheck disable=SC2206
        COMMON_ARGS+=($RRM_CMAKE_ARGS)
    fi
}

# Copies the licence files every package must carry into $1: LICENSE, THIRD_PARTY_NOTICES.md and
# the LICENSES/ folder of full third-party texts (the font licences travel inside assets/fonts).
rrm_copy_legal() {
    local dest="$1" f
    for f in LICENSE THIRD_PARTY_NOTICES.md LICENSES; do
        [[ -e "$RRM_ROOT/$f" ]] || die "missing $f in $RRM_ROOT"
    done
    cp "$RRM_ROOT/LICENSE" "$RRM_ROOT/THIRD_PARTY_NOTICES.md" "$dest/"
    rm -rf "$dest/LICENSES"
    cp -R "$RRM_ROOT/LICENSES" "$dest/LICENSES"
    cp "$RRM_ROOT/packaging/START_HERE.txt" "$dest/START_HERE.txt"
    cp -R "$RRM_ROOT/docs" "$dest/docs"
}

# Copy only the explicit, verified staging bundle; never scan developer installs or game folders.
rrm_copy_bundle() {
    local resources="$1"
    [[ -n "${RRM_BUNDLE_DIR:-}" ]] || return 0
    [[ -f "$RRM_BUNDLE_DIR/manifest.json" ]] || die "RRM_BUNDLE_DIR has no manifest.json"
    mkdir -p "$resources"
    cp -R "$RRM_BUNDLE_DIR" "$resources/bundle"
}

# Warns when the SDL licence texts in LICENSES/ differ from the SDL source built in build dir $1
# (for example after changing RRM_SDL_TAG): copy the new texts into LICENSES/ before releasing.
rrm_check_sdl_licenses() {
    local cache="$1/CMakeCache.txt" src pair
    [[ -f "$cache" ]] || return 0
    src="$(sed -n 's/^SDL3_SOURCE_DIR:[A-Z]*=//p' "$cache")"
    [[ -n "$src" && -d "$src" ]] || return 0
    for pair in LICENSE.txt:SDL3.txt src/hidapi/LICENSE-bsd.txt:SDL3-hidapi.txt \
                src/video/yuv2rgb/LICENSE:SDL3-yuv2rgb.txt; do
        cmp -s "$src/${pair%%:*}" "$RRM_ROOT/LICENSES/${pair#*:}" \
            || log "warning: LICENSES/${pair#*:} differs from SDL's ${pair%%:*}; update it"
    done
}

human_size() { du -h "$1" | cut -f1; }
