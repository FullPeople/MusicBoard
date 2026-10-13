"""Build the source-matched LGPL media runtime with MSVC and an isolated MSYS2.

No system packages are installed. Existing verified downloads are reused.
Run --configure on the first build or whenever configure options change.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import tarfile
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache/minimal-media"
SOURCES = [
    ("ffmpeg-9.0.2.tar.xz", "https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz", "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"),
    ("ffmpeg-9.0.2.tar.xz.asc", "https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz.asc", "d617fd94ea354dadd2a8bb16243d37c44e1e9729b06b6ad58fe30bfb0fa943f2"),
    ("ffmpeg-devel.asc", "https://ffmpeg.org/ffmpeg-devel.asc", "397b3becedcd5a98769967ff1ff8501ddc89f8368b8f766e4701377d7dbaabe5"),
    ("lame_4.0.orig.tar.gz", "https://deb.debian.org/debian/pool/main/l/lame/lame_4.0.orig.tar.gz", "3df5124d5ad3a98312ffd7ba6a9b36230e4f8a3e66d3ce0f425e336c32d216eb"),
    ("lame_4.0-1.dsc", "https://deb.debian.org/debian/pool/main/l/lame/lame_4.0-1.dsc", "1826cba60252354991d65f19da2fea8d357eec2dfe16cc5e47753ab404add6b6"),
]
BUILD_TOOLS = [
    ("msys2-base-x86_64-20260927.tar.xz", "https://repo.msys2.org/distrib/x86_64/msys2-base-x86_64-20260927.tar.xz", "ea2f31a0b6ade63914ce441ffb022f0f6aa96982bfefa2326460a26d5fb01322"),
    ("make-4.4.1-3-x86_64.pkg.tar.zst", "https://repo.msys2.org/msys/x86_64/make-4.4.1-3-x86_64.pkg.tar.zst", "af0bdba17f06fe037f0194069adaa31a8fe45f1a11381501896aea1fae37bd5d"),
    ("nasm-2.16.03-win64.zip", "https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip", "3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6"),
]


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fetch(item):
    name, url, expected = item
    path = CACHE / name
    bundled = ROOT / name
    if not path.exists() and bundled.is_file():
        if sha(bundled) != expected:
            raise RuntimeError("Bundled source digest mismatch: " + name)
        shutil.copy2(bundled, path)
    if not path.exists():
        partial = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(url, timeout=60) as source, partial.open("wb") as target:
            shutil.copyfileobj(source, target, 1024 * 1024)
        if sha(partial) != expected:
            raise RuntimeError("Download digest mismatch: " + name)
        partial.replace(path)
    if sha(path) != expected:
        raise RuntimeError("Cached digest mismatch: " + name)
    return path


def batch(name, lines, vcvars, msys, tools):
    # cmd expansion is kept within validated task-owned paths; no user commands.
    for value in (str(CACHE), str(vcvars)):
        if any(c in value for c in '%"\r\n'):
            raise RuntimeError("Unsupported build path characters")
    content = ["@echo off", "chcp 65001 >nul", 'call "' + str(vcvars) + '" >nul', "if errorlevel 1 exit /b 1",
               'set "VSLANG=1033"', 'set "PATH=%VCToolsInstallDir%bin\\Hostx64\\x64;' + str(msys / "usr/bin") + ';' + str(tools) + ';%PATH%"']
    content += lines
    path = CACHE / name
    path.write_text("\n".join(content) + "\n", encoding="utf-8")
    subprocess.run(["cmd.exe", "/d", "/c", str(path)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configure", action="store_true")
    parser.add_argument("--msys-dir", type=Path, help="Use an existing portable MSYS2 with bash and make")
    args = parser.parse_args()
    CACHE.mkdir(parents=True, exist_ok=True)
    needed_tools = BUILD_TOOLS if args.msys_dir is None else [BUILD_TOOLS[-1]]
    files = {item[0]: fetch(item) for item in SOURCES + needed_tools}
    git = Path(shutil.which("git") or "")
    gpg = git.parent.parent / "usr/bin/gpg.exe"
    homedir = CACHE / "gpg"; homedir.mkdir(exist_ok=True)
    imported = subprocess.run([str(gpg), "--homedir", str(homedir), "--batch", "--import", str(files["ffmpeg-devel.asc"])], capture_output=True)
    verified = subprocess.run([str(gpg), "--homedir", str(homedir), "--batch", "--status-fd", "1", "--verify",
        str(files["ffmpeg-9.0.2.tar.xz.asc"]), str(files["ffmpeg-9.0.2.tar.xz"])], capture_output=True)
    if verified.returncode or b"VALIDSIG FCF986EA15E6E293A5644F10B4322F04D67658D8 " not in verified.stdout:
        raise RuntimeError("FFmpeg source signature verification failed")
    if imported.returncode:
        print("Public key import reported a warning; the expected source signature independently verified.", flush=True)
    for archive, directory in (("ffmpeg-9.0.2.tar.xz", "ffmpeg-9.0.2"), ("lame_4.0.orig.tar.gz", "lame-4.0")):
        if not (CACHE / "sources" / directory).exists():
            with tarfile.open(files[archive]) as package:
                package.extractall(CACHE / "sources", filter="data")
    msys = args.msys_dir.resolve(strict=True) if args.msys_dir else CACHE / "msys-20260927/msys64"
    if args.msys_dir and not all((msys / "usr/bin" / name).is_file() for name in ("bash.exe", "make.exe", "sh.exe")):
        raise RuntimeError("Selected MSYS2 is missing bash, sh or make")
    if not (msys / "usr/bin/bash.exe").exists():
        with tarfile.open(files["msys2-base-x86_64-20260927.tar.xz"]) as package:
            package.extractall(CACHE / "msys-20260927", filter="data")
    if not (msys / "usr/bin/make.exe").exists():
        subprocess.run([str(msys / "usr/bin/bsdtar.exe"), "-xf", str(files["make-4.4.1-3-x86_64.pkg.tar.zst"]), "-C", str(msys)], check=True)
    tools = CACHE / "tools"; tools.mkdir(exist_ok=True)
    import zipfile
    with zipfile.ZipFile(files["nasm-2.16.03-win64.zip"]) as package:
        for name in package.namelist():
            if name.endswith("/nasm.exe"):
                (tools / "nasm.exe").write_bytes(package.read(name))
    for name in ("msvc-cl", "msvc-windres"):
        (tools / name).write_bytes((ROOT / "tools/media-build" / name).read_bytes().replace(b"\r\n", b"\n"))
    subprocess.run(["cmake", "-S", str(ROOT / "tools/media-build/lame"), "-B", str(CACHE / "lame-build"), "-A", "x64",
                    "-DLAME_SOURCE=" + str(CACHE / "sources/lame-4.0")], check=True)
    subprocess.run(["cmake", "--build", str(CACHE / "lame-build"), "--config", "Release", "--parallel", "4"], check=True)
    prefix = CACHE / "install"; (prefix / "include/lame").mkdir(parents=True, exist_ok=True)
    shutil.copy2(CACHE / "sources/lame-4.0/include/lame.h", prefix / "include/lame/lame.h")
    install = subprocess.check_output(["C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe", "-latest", "-products", "*",
                                      "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    vcvars = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
    ffmpeg = CACHE / "sources/ffmpeg-9.0.2"
    options = ["--toolchain=msvc", "--cc=" + (tools / "msvc-cl").as_posix(), "--host-cc=" + (tools / "msvc-cl").as_posix(),
        "--arch=x86_64", "--target-os=win32", "--enable-shared", "--disable-static", "--disable-autodetect", "--enable-schannel",
        "--enable-w32threads", "--enable-libmp3lame", "--disable-doc", "--disable-debug", "--disable-ffplay", "--disable-avdevice",
        "--disable-encoders", "--enable-encoder=libmp3lame,pcm_s16le,pcm_s24le,aac,flac", "--disable-filters",
        "--enable-filter=aformat,anull,aresample,atrim,asetpts,volume", "--prefix=" + prefix.as_posix(),
        "--extra-cflags=-I" + (prefix / "include").as_posix(), "--extra-ldflags=-LIBPATH:" + (CACHE / "lame-build/Release").as_posix()]
    if args.configure or not (ffmpeg / "ffbuild/config.mak").exists():
        batch("configure-reproducible.cmd", ['cd /d "' + str(ffmpeg) + '"', '"' + str(msys / "usr/bin/sh.exe") + '" ./configure ' +
              " ".join('"' + x + '"' for x in options)], vcvars, msys, tools)
    batch("build-reproducible.cmd", ['cd /d "' + str(ffmpeg) + '"', '"' + str(msys / "usr/bin/make.exe") + '" -j4 install ' +
          "WINDRES=" + (tools / "msvc-windres").as_posix() + " SHELL=" + (msys / "usr/bin/sh.exe").as_posix()], vcvars, msys, tools)
    metadata = {"sources": [{"name": name, "url": url, "sha256": digest} for name, url, digest in SOURCES], "configure": options,
        "build_tools": [{"name": name, "url": url, "sha256": digest} for name, url, digest in BUILD_TOOLS],
        "files": {path.name: sha(path) for path in (prefix / "bin").iterdir() if path.suffix in (".exe", ".dll")}}
    (prefix / "provenance.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print("Verified source-matched media runtime: " + str(prefix), flush=True)


if __name__ == "__main__":
    main()
