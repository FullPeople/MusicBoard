"""Fetch pinned Windows media tools and verify published GitHub asset digests."""
from pathlib import Path
import hashlib
import json
import shutil
import urllib.request
import zipfile

root = Path(__file__).resolve().parents[1]
cache = root / ".cache/media-tools"
downloads = root / ".cache/downloads"
cache.mkdir(parents=True, exist_ok=True)
downloads.mkdir(parents=True, exist_ok=True)
tools = [
    {"name": "yt-dlp.exe", "url": "https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp.exe",
     "sha256": "66674953fe251b89f4d08c5f0e35e0728679bd67ab3d7d05c0562af101dd3e7a", "version": "2026.08.19"},
    {"name": "ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip",
     "url": "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip",
     "sha256": "95297f42421807938e51fa7fc65f0a63746b5d6147caef448edbbbd8b71800c5", "version": "9.0"},
]
for tool in tools:
    path = downloads / tool["name"]
    if not path.exists() or hashlib.file_digest(path.open("rb"), "sha256").hexdigest() != tool["sha256"]:
        part = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(tool["url"], timeout=60) as response, part.open("wb") as output:
            shutil.copyfileobj(response, output, 1024 * 1024)
        if hashlib.file_digest(part.open("rb"), "sha256").hexdigest() != tool["sha256"]:
            raise RuntimeError("Published tool digest mismatch: " + tool["name"])
        part.replace(path)
    if path.suffix == ".exe":
        shutil.copy2(path, cache / path.name)
    else:
        with zipfile.ZipFile(path) as package:
            for member in package.infolist():
                name = Path(member.filename)
                if member.is_dir():
                    continue
                if name.parent.name == "bin" and (name.suffix == ".dll" or name.name in {"ffmpeg.exe", "ffprobe.exe"}):
                    target = cache / name.name
                elif name.name.lower().startswith(("license", "copying")) or name.name == "buildconfig.txt":
                    target = cache / "licenses" / name.name
                    target.parent.mkdir(exist_ok=True)
                else:
                    continue
                with package.open(member) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output, 1024 * 1024)
    print("Verified", tool["name"], flush=True)
(cache / "provenance.json").write_text(json.dumps({"upstream": tools,
    "files": {str(p.relative_to(cache)).replace("\\", "/"): hashlib.file_digest(p.open("rb"), "sha256").hexdigest()
        for p in cache.rglob("*") if p.is_file() and p.name != "provenance.json"}}, indent=2), encoding="utf-8")
print("Verified media tools ready:", cache, flush=True)
