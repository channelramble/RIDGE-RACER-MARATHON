#!/usr/bin/env python3
"""Offline fixtures exercise the production importer, including hostile ZIPs."""
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import zipfile

tool = Path(sys.argv[1]).resolve()
catalog = Path(sys.argv[2]).resolve()
platform = subprocess.check_output([str(tool), "--platform"], text=True).strip()


with tempfile.TemporaryDirectory(prefix="rrm-bundle-test-") as temporary:
    root = Path(temporary)
    bundle = root / "read-only bundle"
    bundle.mkdir()
    archive = bundle / "backend.zip"
    manifest = bundle / "manifest.json"
    entry = dict(id="lr-swanstation", version="fixture-1", archive=archive.name,
                 executable="bin/core", sha256="")

    def pack(files=None, links=None, edits=None):
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
            for name, content in (files or {"bin/core": b"fixture executable"}).items():
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | 0o755) << 16
                output.writestr(info, content)
            for name, target in (links or {}).items():
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = (stat.S_IFLNK | 0o777) << 16
                output.writestr(info, target)
        item = dict(entry, sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
        item.update(edits or {})
        manifest.write_text(json.dumps(dict(schema=1, platform=platform, entries=[item])))

    def run(name, success=True, verify=False):
        data = root / name
        args = [str(tool), "--bundle", str(bundle), "--data", str(data), "--catalog", str(catalog)]
        if verify:
            args.append("--verify-only")
        result = subprocess.run(args, text=True, capture_output=True)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        parsed = json.loads(result.stdout)
        return data, parsed

    pack({"bin/core": b"fixture executable", "system/PPSSPP/asset.txt": b"asset"},
         edits={"system_files": [dict(source="system/PPSSPP", destination="libretro/PPSSPP")]})
    data, result = run("verify", verify=True)
    assert not (data / "library.json").exists()
    assert not any((data / "backends").iterdir())
    # The release location can be read-only; every write belongs to --data.
    if os.name != "nt":
        bundle.chmod(0o555)
        archive.chmod(0o444)
        manifest.chmod(0o444)
    data, result = run("good")
    assert result["imported"] == [entry["id"]]
    installed = json.loads((data / "library.json").read_text())["installs"][entry["id"]]
    exe = Path(installed["executable"])
    assert exe.read_bytes() == b"fixture executable"
    assert (data / "system/libretro/PPSSPP/asset.txt").read_bytes() == b"asset"
    if os.name != "nt":
        assert exe.stat().st_mode & stat.S_IXUSR
        bundle.chmod(0o755)
        archive.chmod(0o644)
        manifest.chmod(0o644)
    exe.write_bytes(b"user changed this")
    _, result = run("good")
    assert result["skipped"] == [entry["id"]]
    assert exe.read_bytes() == b"user changed this"
    saved = json.loads((data / "library.json").read_text())
    saved["installs"][entry["id"]]["userProvided"] = True
    saved["installs"][entry["id"]]["executable"] = str(root / "temporarily unplugged drive/core")
    (data / "library.json").write_text(json.dumps(saved))
    _, result = run("good")
    assert result["skipped"] == [entry["id"]], "Missing user-linked installs must be preserved"

    pack(edits={"sha256": "0" * 64})
    run("bad-hash", success=False)
    pack(edits={"archive": "../backend.zip"})
    run("bad-manifest-path", success=False)
    pack({"../escaped": b"bad", "bin/core": b"exe"})
    run("zip-traversal", success=False)
    assert not (root / "escaped").exists()
    pack({"bin/core": b"exe"}, links={"evil": "../../escaped"})
    run("link-traversal", success=False)
    # Each link appears lexically inside the ZIP; their combined resolution escapes.
    pack({"bin/core": b"exe"}, links={"a/root": "..", "escape": "a/root/.."})
    run("chained-link-traversal", success=False)
    pack({"bin/core": b"exe", "alias/injected": b"bad"}, links={"alias": "bin"})
    run("write-through-link", success=False)
    pack(edits={"executable": "missing"})
    run("missing-exe", success=False, verify=True)
    pack(edits={"system_files": [dict(source="bin", destination="../../escape")]})
    run("system-traversal", success=False)
    pack()
    parsed = json.loads(manifest.read_text())
    parsed["platform"] = "wrong-platform"
    manifest.write_text(json.dumps(parsed))
    run("wrong-platform", success=False)
    # A normal internal framework-style link is preserved when the OS supports it.
    if os.name != "nt":
        pack({"versions/A/core": b"exe"}, links={"versions/current": "A", "bin/core": "../versions/current/core"})
        data, result = run("valid-links")
        installed = json.loads((data / "library.json").read_text())["installs"][entry["id"]]
        assert Path(installed["executable"]).is_symlink()
        assert Path(installed["executable"]).read_bytes() == b"exe"

    if platform.startswith("macos-"):
        # PPSSPP must isolate its preferences and re-sign the writable copy before
        # registration. A malformed upstream app is a failed import, also on retry.
        pack({"PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL": b"fixture executable",
              "PPSSPPSDL.app/Contents/Info.plist": b"not a plist"},
             edits={"id": "ppsspp", "executable": "PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL"})
        for _ in range(2):
            data, result = run("ppsspp-preparation-failure", success=False)
            assert not result["imported"]
            assert any("local backend setup" in error for error in result["errors"])
            assert not (data / "library.json").exists(), "Failed preparation must not register an install"

print("Backend bundle fixtures passed (verification, read-only source, import, no clobber, hashes, paths and links).")
