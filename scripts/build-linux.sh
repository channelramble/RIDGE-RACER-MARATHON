#!/usr/bin/env bash
# Builds Ridge Racer Marathon natively on Linux and packages
#   dist/RidgeRacerMarathon-linux-x64.tar.gz   (linux-arm64 on aarch64)
#     RidgeRacerMarathon/bin/RidgeRacerMarathon
#     RidgeRacerMarathon/share/ridge-racer-marathon/{catalog,assets}
#     RidgeRacerMarathon/share/applications/io.github.ridgeracermarathon.desktop
#     RidgeRacerMarathon/share/icons/hicolor/256x256/apps/io.github.ridgeracermarathon.png
#     RidgeRacerMarathon/share/doc/ridge-racer-marathon/{LICENSE,THIRD_PARTY_NOTICES.md,LICENSES/}
#     RidgeRacerMarathon/{LICENSE,THIRD_PARTY_NOTICES.md,LICENSES/}
# The tree is CMake's own install layout (bin/ and share/ can be copied into ~/.local or /usr/local).
#
# Requirements: cmake >= 3.24, ninja (optional), GCC 11+ or Clang 14+, git, and the headers SDL needs
# for X11, Wayland and audio. Ubuntu / Debian:
#   sudo apt install build-essential cmake git && .github/scripts/install-linux-deps.sh
# (other distros: see docs/README-linux.md in the SDL source). SDL loads those libraries at runtime
# when present, so the binary itself only needs the C/C++ runtime. Build on the oldest distro you
# want to support: the binary needs at least the glibc it was built against.
source "$(dirname "$0")/common.sh"

[[ "$(uname -s)" == Linux ]] || die "build-linux.sh runs on Linux only (use build-windows.sh / build-macos.sh elsewhere)"
case "$(uname -m)" in
    x86_64) ARCH=x64 ;;
    aarch64 | arm64) ARCH=arm64 ;;
    *) die "unsupported CPU $(uname -m)" ;;
esac
command -v tar >/dev/null 2>&1 || die "tar not found"

BUILD_DIR="${BUILD_DIR:-$RRM_ROOT/build-dist/linux-$ARCH}"
ARTIFACT="$DIST_DIR/RidgeRacerMarathon-linux-$ARCH.tar.gz"

rrm_common_args
log "Configuring ($BUILD_DIR)"
cmake -S "$RRM_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DRRM_VENDORED_SDL=ON \
    -DRRM_BUILD_TESTS=OFF \
    ${COMMON_ARGS[@]+"${COMMON_ARGS[@]}"}

log "Building"
cmake --build "$BUILD_DIR" --target RidgeRacerMarathon --parallel "$JOBS"
rrm_check_sdl_licenses "$BUILD_DIR"

STAGE="$BUILD_DIR/package"
PKGDIR="$STAGE/RidgeRacerMarathon"
rm -rf "$STAGE"
mkdir -p "$PKGDIR"
# CMake's install rules (CMakeLists.txt) lay out bin/ and share/, including the licence files.
cmake --install "$BUILD_DIR" --prefix "$PKGDIR" --strip
rrm_copy_legal "$PKGDIR"
rrm_copy_bundle "$PKGDIR/share/ridge-racer-marathon"
find "$PKGDIR" -name .DS_Store -delete

EXE="$PKGDIR/bin/RidgeRacerMarathon"
for f in "$EXE" \
         "$PKGDIR/share/ridge-racer-marathon/catalog/games.json" \
         "$PKGDIR/share/ridge-racer-marathon/assets/fonts/Rajdhani-OFL.txt" \
         "$PKGDIR/share/applications/io.github.ridgeracermarathon.desktop" \
         "$PKGDIR/share/icons/hicolor/256x256/apps/io.github.ridgeracermarathon.png" \
         "$PKGDIR/share/doc/ridge-racer-marathon/LICENSE" \
         "$PKGDIR/share/doc/ridge-racer-marathon/THIRD_PARTY_NOTICES.md" \
         "$PKGDIR/share/doc/ridge-racer-marathon/LICENSES/SDL3.txt"; do
    [[ -f "$f" ]] || die "install did not produce ${f#"$PKGDIR"/}"
done

# SDL is linked statically and loads X11 / Wayland / audio libraries itself, so the binary must
# only need the C/C++ runtime (no libSDL3.so).
if command -v readelf >/dev/null 2>&1; then
    NEEDED="$(readelf -d "$EXE" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort -u)"
    log "Needed libraries: $(echo "$NEEDED" | tr '\n' ' ')"
    if echo "$NEEDED" | grep -Ev '^(libc|libm|libdl|libpthread|librt|libstdc\+\+|libgcc_s|ld-linux[^ ]*)\.so' | grep -q .; then
        die "unexpected shared library dependency"
    fi
    GLIBC_MIN="$(readelf -V "$EXE" | grep -o 'GLIBC_[0-9][0-9.]*' | sort -uV | tail -n1 || true)"
    log "Newest glibc symbol version used: ${GLIBC_MIN:-unknown}"
fi

rm -f "$ARTIFACT"
tar -C "$STAGE" --owner=0 --group=0 --numeric-owner -czf "$ARTIFACT" RidgeRacerMarathon
log "Wrote $ARTIFACT ($(human_size "$ARTIFACT"))"
