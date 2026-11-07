#!/usr/bin/env python3
"""Package an OTA release (PLAN §12). Owner: notify_ota.

Run after `pio run -e c3`:
    python3 tools/make_release.py [--notes "text"] [--version 1.4.0]

Writes dist/firmware-<ver>.bin and dist/manifest.json, then prints the `gh release create` command.
Version defaults to `git describe --tags` (leading "v" stripped), matching tools/version.py.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

REPO = "sergesyrota/charger-cost-control"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, ".pio", "build", "c3", "firmware.bin")
DIST = os.path.join(ROOT, "dist")
SLOT = 0x190000  # partitions.csv app slot size


def git_version():
    try:
        v = subprocess.check_output(["git", "describe", "--tags", "--always", "--dirty"],
                                    cwd=ROOT, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        sys.exit("git describe failed; pass --version")
    return v[1:] if v.startswith("v") else v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", help="override version (default: git describe --tags)")
    ap.add_argument("--notes", default="", help="release notes for the manifest")
    a = ap.parse_args()

    ver = a.version or git_version()
    if not os.path.isfile(BIN):
        sys.exit(f"{BIN} not found; run `pio run -e c3` first")
    if "dirty" in ver or "-g" in ver:
        print(f"WARNING: version '{ver}' is not a clean tag; tag the commit (git tag v1.2.3) and rebuild.",
              file=sys.stderr)

    data = open(BIN, "rb").read()
    if len(data) > SLOT:
        sys.exit(f"image {len(data)} B exceeds app slot {SLOT} B")
    name = f"firmware-{ver}.bin"
    os.makedirs(DIST, exist_ok=True)
    out_bin = os.path.join(DIST, name)
    shutil.copyfile(BIN, out_bin)

    manifest = {
        "version": ver,
        "url": f"https://github.com/{REPO}/releases/download/v{ver}/{name}",
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
        "notes": a.notes,
    }
    out_man = os.path.join(DIST, "manifest.json")
    with open(out_man, "w") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")

    print(f"{out_bin}  {len(data)} B ({100.0 * len(data) / SLOT:.1f}% of slot)")
    print(f"{out_man}  sha256={manifest['sha256']}")
    notes = a.notes or f"Firmware {ver}"
    print("\nPublish with:")
    print(f"  gh release create v{ver} {os.path.relpath(out_bin, ROOT)} {os.path.relpath(out_man, ROOT)} "
          f"--repo {REPO} --title v{ver} --notes {json.dumps(notes)}")


if __name__ == "__main__":
    main()
