#!/usr/bin/env python3
"""Verify the actual first-run import performed by the packaged app."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys


def load_core(path):
    class Info(ctypes.Structure):
        _fields_ = [("name", ctypes.c_char_p), ("version", ctypes.c_char_p),
                    ("extensions", ctypes.c_char_p), ("need_fullpath", ctypes.c_bool),
                    ("block_extract", ctypes.c_bool)]
    dll_directory = os.add_dll_directory(str(path.parent)) if os.name == "nt" else None
    core = ctypes.CDLL(str(path))
    core.retro_get_system_info.argtypes = [ctypes.POINTER(Info)]
    info = Info()
    core.retro_get_system_info(ctypes.byref(info))
    assert info.name and info.version, f"Invalid libretro metadata: {path}"
    print(info.name.decode(), info.version.decode(), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle", type=Path, nargs="?")
    parser.add_argument("data", type=Path, nargs="?")
    parser.add_argument("--load-core", type=Path)
    args = parser.parse_args()
    if args.load_core:
        load_core(args.load_core)
        return
    manifest = json.loads((args.bundle / "manifest.json").read_text())
    lock = json.loads((args.bundle / "backend-lock.json").read_text())
    expected = {b["id"] for b in lock["binaries"] if b["platform"] == manifest["platform"]}
    actual = [b["id"] for b in manifest["entries"]]
    assert set(actual) == expected and len(actual) == len(expected), "Bundle differs from reviewed backend inventory"
    library = json.loads((args.data / "library.json").read_text())
    assert manifest["entries"], "Complete package contains no backends"
    assert not library["games"], "Fresh setup unexpectedly contains games"
    assert not library["system"], "Fresh setup unexpectedly contains firmware"
    for entry in manifest["entries"]:
        installed = library["installs"].get(entry["id"])
        assert installed, f"Backend was not imported: {entry['id']}"
        root = Path(installed["root"]).resolve()
        executable = Path(installed["executable"]).resolve()
        assert executable.is_file(), f"Missing executable: {executable}"
        assert root in executable.parents, f"Executable escaped install: {executable}"
        assert args.data.resolve() in root.parents, f"Install outside user data: {root}"
        assert not installed["userProvided"], "Imported backend marked as user-provided"
        if os.name != "nt":
            assert os.access(executable, os.X_OK), f"Executable lost its mode: {executable}"
        if executable.name.endswith((".dylib", ".dll", ".so")):
            subprocess.run([sys.executable, __file__, "--load-core", str(executable)],
                           check=True, timeout=60)
        print(f"Verified offline import: {entry['id']} ({installed['version']})", flush=True)
    print(f"PASS: {len(manifest['entries'])} backends imported from the release package")


if __name__ == "__main__":
    main()
