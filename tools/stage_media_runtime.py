"""Stage the verified source-matched runtime beside the existing yt-dlp.

Writes a separate cache directory. Switching the active cache is a separate
reviewable operation after runtime and public-link validation.
"""
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

from build_media_runtime import ROOT, CACHE, SOURCES, sha


def main():
    current = ROOT / ".cache/media-tools"
    stage = ROOT / ".cache/media-tools-lgpl"
    stage.mkdir(exist_ok=True)
    install = CACHE / "install"
    metadata = json.loads((install / "provenance.json").read_text(encoding="utf-8"))
    for name, digest in metadata["files"].items():
        source = install / "bin" / name
        if sha(source) != digest:
            raise RuntimeError("Runtime binary digest mismatch: " + name)
        shutil.copy2(source, stage / name)
    yt = current / "yt-dlp.exe"
    expected = "66674953fe251b89f4d08c5f0e35e0728679bd67ab3d7d05c0562af101dd3e7a"
    if sha(yt) != expected:
        raise RuntimeError("yt-dlp digest mismatch")
    shutil.copy2(yt, stage / yt.name)
    licenses = stage / "licenses"; licenses.mkdir(exist_ok=True)
    for name in ("yt-dlp-UNLICENSE.txt", "yt-dlp-THIRD_PARTY_LICENSES.txt"):
        shutil.copy2(current / "licenses" / name, licenses / name)
    shutil.copy2(CACHE / "sources/ffmpeg-9.0.2/COPYING.LGPLv2.1", licenses / "FFmpeg-LGPL-2.1.txt")
    shutil.copy2(CACHE / "sources/lame-4.0/COPYING", licenses / "LAME-COPYING.txt")
    sources = stage / "sources"; sources.mkdir(exist_ok=True)
    archive = sources / "MusicBoard-media-corresponding-source.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as package:
        for name, _, digest in SOURCES:
            if sha(CACHE / name) != digest:
                raise RuntimeError("Corresponding source digest mismatch: " + name)
            package.write(CACHE / name, name)
        package.write(ROOT / "tools/build_media_runtime.py", "tools/build_media_runtime.py")
        for path in (ROOT / "tools/media-build").rglob("*"):
            if path.is_file():
                package.write(path, path.relative_to(ROOT).as_posix())
        package.write(install / "provenance.json", "provenance.json")
        package.write(licenses / "FFmpeg-LGPL-2.1.txt", "LICENSES/FFmpeg-LGPL-2.1.txt")
        package.write(licenses / "LAME-COPYING.txt", "LICENSES/LAME-COPYING.txt")
        package.write(ROOT / "tools/media-build/README.txt", "README.txt")
    metadata["yt-dlp"] = {"version": "2026.08.19", "sha256": expected,
        "url": "https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp.exe"}
    metadata["files"] = {path.relative_to(stage).as_posix(): sha(path) for path in stage.rglob("*")
                          if path.is_file() and path.name != "provenance.json"}
    (stage / "provenance.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(json.dumps({"Directory": str(stage), "Bytes": sum(path.stat().st_size for path in stage.rglob("*") if path.is_file()),
                      "SourceSHA256": sha(archive)}), flush=True)


if __name__ == "__main__":
    main()
