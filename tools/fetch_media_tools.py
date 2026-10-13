"""Fetch verified yt-dlp files and optionally build the source-matched audio runtime.

Use --build-audio for the complete release prerequisite. No general-purpose
prebuilt FFmpeg package is downloaded or used as a release fallback.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
from build_media_runtime import ROOT, sha

FILES = [
    ("yt-dlp.exe", "https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp.exe", "66674953fe251b89f4d08c5f0e35e0728679bd67ab3d7d05c0562af101dd3e7a"),
    ("licenses/yt-dlp-UNLICENSE.txt", "https://raw.githubusercontent.com/yt-dlp/yt-dlp/2026.08.19/LICENSE", "7e12e5df4bae12cb21581ba157ced20e1986a0508dd10d0e8a4ab9a4cf94e85c"),
    ("licenses/yt-dlp-THIRD_PARTY_LICENSES.txt", "https://raw.githubusercontent.com/yt-dlp/yt-dlp/2026.08.19/THIRD_PARTY_LICENSES.txt", "472aefe951c7db35e1657c1d13fd337140511ed6f2b329205105ad441c5a02b7"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-audio", action="store_true")
    options = parser.parse_args()
    cache = ROOT / ".cache/media-tools"
    cache.mkdir(parents=True, exist_ok=True)
    for name, url, expected in FILES:
        target = cache / name
        target.parent.mkdir(exist_ok=True)
        if not target.exists():
            partial = target.with_suffix(target.suffix + ".part")
            with urllib.request.urlopen(url, timeout=60) as source, partial.open("wb") as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
            if sha(partial) != expected:
                raise RuntimeError("Download digest mismatch: " + name)
            partial.replace(target)
        if sha(target) != expected:
            raise RuntimeError("Cached component digest mismatch: " + name)
    if options.build_audio:
        subprocess.run([sys.executable, str(ROOT / "tools/build_media_runtime.py")], check=True)
        subprocess.run([sys.executable, str(ROOT / "tools/stage_media_runtime.py")], check=True)
        stage = ROOT / ".cache/media-tools-lgpl"
        # Existing runtime files are updated only after staging and digest checks.
        metadata = json.loads((stage / "provenance.json").read_text(encoding="utf-8"))
        for name, digest in metadata["files"].items():
            if sha(stage / name) != digest:
                raise RuntimeError("Staged component digest mismatch: " + name)
        for name in metadata["files"]:
            target = cache / name; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(stage / name, target)
        shutil.copy2(stage / "provenance.json", cache / "provenance.json")
    print("Verified yt-dlp files. Complete audio release prerequisites: --build-audio", flush=True)


if __name__ == "__main__":
    main()
