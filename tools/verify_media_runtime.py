"""Verify the exact, source-matched release media files without executing them."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = {"ffmpeg.exe", "ffprobe.exe", "avcodec-63.dll", "avfilter-12.dll", "avformat-63.dll",
           "avutil-61.dll", "swresample-7.dll", "swscale-10.dll", "yt-dlp.exe"}
SUPPORT = {"licenses/yt-dlp-UNLICENSE.txt", "licenses/yt-dlp-THIRD_PARTY_LICENSES.txt",
           "licenses/FFmpeg-LGPL-2.1.txt", "licenses/LAME-COPYING.txt",
           "sources/MusicBoard-media-corresponding-source.zip"}


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify(directory):
    metadata = json.loads((directory / "provenance.json").read_text(encoding="utf-8"))
    files = metadata["files"]
    if set(files) != RUNTIME | SUPPORT:
        raise RuntimeError("Media provenance must contain exactly the approved runtime, licenses and corresponding source")
    actual = {path.relative_to(directory).as_posix() for path in directory.rglob("*") if path.is_file()}
    if actual != RUNTIME | SUPPORT | {"provenance.json"}:
        raise RuntimeError("Unrecorded or missing media files; preserve the cache and rebuild into a separate directory")
    for name, expected in files.items():
        parts = PurePosixPath(name).parts
        if not parts or any(part in ("", ".", "..") for part in parts) or sha(directory / name) != expected:
            raise RuntimeError("Media file digest mismatch: " + name)
    options = metadata["configure"]
    if "--disable-autodetect" not in options or "--enable-shared" not in options or any(
            option.startswith(("--enable-gpl", "--enable-nonfree")) for option in options):
        raise RuntimeError("Unexpected media build configuration")
    source = directory / "sources/MusicBoard-media-corresponding-source.zip"
    with zipfile.ZipFile(source) as archive:
        if archive.testzip() is not None:
            raise RuntimeError("Corresponding source archive is corrupt")
        for item in metadata["sources"]:
            if hashlib.sha256(archive.read(item["name"])).hexdigest() != item["sha256"]:
                raise RuntimeError("Corresponding source digest mismatch: " + item["name"])
        for name in ("tools/build_media_runtime.py", "tools/media-build/msvc-cl", "tools/media-build/msvc-windres",
                     "tools/media-build/lame/CMakeLists.txt", "README.txt", "LICENSES/FFmpeg-LGPL-2.1.txt", "LICENSES/LAME-COPYING.txt"):
            if not archive.read(name):
                raise RuntimeError("Missing corresponding build recipe or license: " + name)
    return {"Passed": True, "Files": len(files), "SourceSHA256": sha(source)}


if __name__ == "__main__":
    print(json.dumps(verify(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / ".cache/media-tools")))
