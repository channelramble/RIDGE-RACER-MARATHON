#!/usr/bin/env bash
# Builds Ridge Racer Marathon for macOS (Apple Silicon), signs it and packages
#   dist/RidgeRacerMarathon-macos-arm64.zip
#     RidgeRacerMarathon/Ridge Racer Marathon.app, LICENSE, THIRD_PARTY_NOTICES.md, LICENSES/
#     (the app carries the same licence files in Contents/Resources)
#
# Requirements: Xcode command line tools, cmake >= 3.24, ninja (optional).
#
# Signing knobs (besides those in common.sh):
#   CODESIGN_IDENTITY    e.g. "Developer ID Application: Name (TEAMID)". Signs with the hardened
#                        runtime and a secure timestamp. Unset: ad-hoc signature (runs on this Mac;
#                        other Macs must allow it once under System Settings > Privacy & Security).
#   NOTARY_PROFILE       notarytool keychain profile (xcrun notarytool store-credentials). When set,
#                        the signed app is notarized and the ticket stapled. Needs CODESIGN_IDENTITY.
#   MACOS_DEPLOYMENT_TARGET  oldest macOS the build runs on (default: 11.0, as in Info.plist)
source "$(dirname "$0")/common.sh"

[[ "$(uname -s)" == Darwin ]] || die "build-macos.sh runs on macOS only"
for tool in codesign ditto xcrun; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool not found (install the Xcode command line tools)"
done
if [[ -n "${NOTARY_PROFILE:-}" && -z "${CODESIGN_IDENTITY:-}" ]]; then
    die "NOTARY_PROFILE needs CODESIGN_IDENTITY (Apple does not notarize ad-hoc signatures)"
fi

BUILD_DIR="${BUILD_DIR:-$RRM_ROOT/build-dist/macos-arm64}"
ARTIFACT="$DIST_DIR/RidgeRacerMarathon-macos-arm64.zip"
APP_NAME="Ridge Racer Marathon.app"

rrm_common_args
log "Configuring ($BUILD_DIR)"
cmake -S "$RRM_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-11.0}" \
    -DRRM_VENDORED_SDL=ON \
    -DRRM_BUILD_TESTS=OFF \
    ${COMMON_ARGS[@]+"${COMMON_ARGS[@]}"}

log "Building"
cmake --build "$BUILD_DIR" --target RidgeRacerMarathon --parallel "$JOBS"

BUILT_APP="$BUILD_DIR/$APP_NAME"
[[ -d "$BUILT_APP" ]] || die "build did not produce $BUILT_APP"
# CMake copies the licence files into the bundle (see CMakeLists.txt); make sure it did.
for f in LICENSE THIRD_PARTY_NOTICES.md LICENSES/SDL3.txt; do
    [[ -f "$BUILT_APP/Contents/Resources/$f" ]] || die "$f missing from the app's Contents/Resources"
done
rrm_check_sdl_licenses "$BUILD_DIR"

# The executable must only link system libraries (SDL is static; no Homebrew or @rpath dylibs).
EXE="$BUILT_APP/Contents/MacOS/Ridge Racer Marathon"
LINKED="$(otool -L "$EXE" | tail -n +2 | awk '{print $1}')"
if echo "$LINKED" | grep -Ev '^(/System/Library/|/usr/lib/)' | grep -q .; then
    die "unexpected non-system library: $(echo "$LINKED" | grep -Ev '^(/System/Library/|/usr/lib/)' | tr '\n' ' ')"
fi

STAGE="$BUILD_DIR/package"
PKGDIR="$STAGE/RidgeRacerMarathon"
APP="$PKGDIR/$APP_NAME"
rm -rf "$STAGE"
mkdir -p "$PKGDIR"
ditto "$BUILT_APP" "$APP"
rrm_copy_legal "$PKGDIR"
rrm_copy_bundle "$APP/Contents/Resources"
find "$PKGDIR" -name .DS_Store -delete
xattr -cr "$APP"

# Libretro cores run inside Marathon: under the hardened runtime they need JIT memory and must load
# although they are signed by someone else.
ENTITLEMENTS="$BUILD_DIR/marathon.entitlements"
cat > "$ENTITLEMENTS" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>com.apple.security.cs.allow-jit</key>
	<true/>
	<key>com.apple.security.cs.allow-unsigned-executable-memory</key>
	<true/>
	<key>com.apple.security.cs.disable-library-validation</key>
	<true/>
</dict>
</plist>
EOF

# Nested code first, then the bundle.
if [[ -n "${CODESIGN_IDENTITY:-}" ]]; then
    log "Signing with \"$CODESIGN_IDENTITY\" (hardened runtime)"
    SIGN=(codesign --force --timestamp --options runtime --sign "$CODESIGN_IDENTITY")
else
    log "Signing ad-hoc (set CODESIGN_IDENTITY for a Developer ID signature)"
    SIGN=(codesign --force --sign -)
fi
while IFS= read -r -d '' lib; do
    "${SIGN[@]}" "$lib"
done < <(find "$APP/Contents/Frameworks" -type f -name '*.dylib' -print0 2>/dev/null)
"${SIGN[@]}" --entitlements "$ENTITLEMENTS" "$APP"
codesign --verify --strict --deep --verbose=2 "$APP"

if [[ -n "${NOTARY_PROFILE:-}" ]]; then
    log "Notarizing (profile $NOTARY_PROFILE)"
    SUBMIT_ZIP="$STAGE/notarize.zip"
    ditto -c -k --norsrc --keepParent "$APP" "$SUBMIT_ZIP"
    NOTARY_OUT="$(xcrun notarytool submit "$SUBMIT_ZIP" --keychain-profile "$NOTARY_PROFILE" --wait)" \
        || die "notarytool submit failed"
    echo "$NOTARY_OUT" >&2
    echo "$NOTARY_OUT" | grep -q 'status: Accepted' \
        || die "notarization was not accepted (see: xcrun notarytool log <id> --keychain-profile $NOTARY_PROFILE)"
    rm -f "$SUBMIT_ZIP"
    xcrun stapler staple "$APP"
    xcrun stapler validate "$APP"
    spctl --assess --type execute --verbose=2 "$APP"
fi

rm -f "$ARTIFACT"
ditto -c -k --norsrc --keepParent "$PKGDIR" "$ARTIFACT"

# The signature must survive the zip.
CHECK="$(mktemp -d)"
ditto -x -k "$ARTIFACT" "$CHECK"
codesign --verify --strict --deep "$CHECK/RidgeRacerMarathon/$APP_NAME" || die "signature broken in $ARTIFACT"
rm -rf "$CHECK"
log "Wrote $ARTIFACT ($(human_size "$ARTIFACT"))"
