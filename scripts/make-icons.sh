#!/usr/bin/env bash
# Regenerates all app icons from the vector design in scripts/make-icons.swift.
# Requires macOS (swift, CoreGraphics, iconutil). The generated files are committed,
# so other platforms and CI never need to run this.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

swift "$ROOT/scripts/make-icons.swift" "$ROOT"
iconutil -c icns "$ROOT/platform/icons/Marathon.iconset" -o "$ROOT/platform/macos/Marathon.icns"
rm -rf "$ROOT/platform/icons/Marathon.iconset"
echo "macOS icon: platform/macos/Marathon.icns"
