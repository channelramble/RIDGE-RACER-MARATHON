#!/usr/bin/env bash
# Run after smoke-app.sh, reusing its virtual display support / Windows Mesa installation.
# Exercises the real UI with virtual gamepads; no games or backend downloads are needed.
set -euo pipefail

tool="$1"
out="$(cd "$2" && pwd)"
run=("$tool")
case "$(uname -s)" in
    Linux)
        export LIBGL_ALWAYS_SOFTWARE=1
        run=(xvfb-run -a -s "-screen 0 1280x800x24" "$tool")
        ;;
    MINGW* | MSYS* | CYGWIN*)
        # smoke-app.sh copies the complete build output beside Mesa's opengl32.dll.
        export GALLIUM_DRIVER=llvmpipe
        run=("$out/app/$(basename "$tool").exe")
        ;;
esac

"${run[@]}" --png "$out/controllers.png"
[[ -s "$out/controllers.png" ]] || { echo "No controller UI screenshot was written" >&2; exit 1; }
