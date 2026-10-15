#!/usr/bin/env bash
# Ubuntu packages for building Marathon on Linux with SDL3 built from source
# (-DRRM_VENDORED_SDL=ON or no system SDL3). SDL loads X11, Wayland, audio and
# input libraries at run time; it only needs their headers to build support in.
set -euo pipefail

packages=(
    ninja-build pkg-config
    # video: X11, Wayland, OpenGL / EGL / GLES, KMS/DRM
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev
    libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev
    libgl-dev libegl-dev libgles-dev libdrm-dev libgbm-dev
    # audio
    libasound2-dev libpulse-dev libpipewire-0.3-dev
    # input, hotplug, IME
    libdbus-1-dev libudev-dev libibus-1.0-dev
)

sudo apt-get update -q
sudo apt-get install -y -q --no-install-recommends "${packages[@]}"
