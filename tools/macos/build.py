"""Build the native Mac app and source-matched LGPL helper on a Mac runner."""
from pathlib import Path
import hashlib, json, os, platform, plistlib, shutil, subprocess, tarfile, urllib.request

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'artifacts/macos'
CACHE = ROOT / '.cache/macos'
VERSION = '0.3.6'
SOURCES = [
    ('ffmpeg-9.0.2.tar.xz', 'https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz', '8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e'),
    ('yt-dlp', 'https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp_macos', '0f192b7ec147ab6288885d6351d9ab67367640029b4377576ef46dd79cf7b202'),
]

def sha(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()

def run(*args, **kw):
    subprocess.run([str(x) for x in args], check=True, **kw)

def fetch(name, url, digest):
    path = CACHE / name
    if not path.exists():
        with urllib.request.urlopen(url, timeout=90) as src, path.open('wb') as dst: shutil.copyfileobj(src, dst)
    if sha(path) != digest: raise RuntimeError('Source digest mismatch: ' + name)
    return path

def main():
    if platform.system() != 'Darwin': raise RuntimeError('Build requires native macOS')
    OUT.mkdir(parents=True, exist_ok=False); CACHE.mkdir(parents=True, exist_ok=True)
    app = OUT / 'MusicBoard.app'; mac = app/'Contents/MacOS'; resources = app/'Contents/Resources'; tools = resources/'media-tools'
    mac.mkdir(parents=True); tools.mkdir(parents=True)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    swift = [str(ROOT/'macos'/name) for name in ['AudioBoard.swift','BoardView.swift','Acceptance.swift','main.swift']]
    run('xcrun','swiftc','-swift-version','5','-O','-sdk',sdk,'-target',platform.machine()+'-apple-macosx13.0',*swift,'-framework','AppKit','-framework','SwiftUI','-framework','AVFoundation','-o',mac/'MusicBoard')
    paths = [fetch(*item) for item in SOURCES]
    source = CACHE / 'ffmpeg-9.0.2'
    if not source.exists():
        with tarfile.open(paths[0]) as archive: archive.extractall(CACHE, filter='data')
    prefix = CACHE / 'media-runtime'
    configure = ['--prefix=' + str(prefix), '--disable-shared', '--enable-static', '--disable-debug', '--disable-doc', '--disable-autodetect', '--disable-x86asm', '--disable-everything', '--enable-ffmpeg', '--enable-ffprobe', '--enable-swresample', '--enable-avfilter', '--enable-protocol=file,pipe', '--enable-demuxer=mov,matroska,ogg,wav,mp3,flac,aiff,asf,aac', '--enable-decoder=aac,alac,flac,mp3,mp3float,opus,vorbis,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le,pcm_s16be,pcm_s24be,pcm_s32be,wmav1,wmav2', '--enable-parser=aac,mpegaudio,opus,vorbis,flac', '--enable-muxer=ipod,mov,mp3,wav,caf,flac,adts', '--enable-encoder=aac,alac,flac,pcm_s16le,pcm_f32le', '--enable-filter=aresample,anull,aformat,volume,atrim,asetpts', '--extra-cflags=-mmacosx-version-min=13.0', '--extra-ldflags=-mmacosx-version-min=13.0']
    run(source / 'configure', *configure, cwd=source, stdout=(OUT/'ffmpeg-configure.txt').open('w'))
    with (OUT/'ffmpeg-build.txt').open('w') as log: run('make', '-j3', 'install', cwd=source, stdout=log, stderr=subprocess.STDOUT)
    for name in ['ffmpeg', 'ffprobe']: shutil.copy2(prefix/'bin'/name, tools/name)
    shutil.copy2(paths[1], tools/'yt-dlp'); (tools/'yt-dlp').chmod(0o755)
    icons = OUT/'MusicBoard.iconset'; icons.mkdir()
    for size in [16,32,128,256,512]:
        for retina in [False,True]:
            target = icons/(f'icon_{size}x{size}'+('@2x' if retina else '')+'.png')
            run('sips','-z',size*(2 if retina else 1),size*(2 if retina else 1),ROOT/'assets/icons/MusicBoard.png','--out',target,stdout=subprocess.DEVNULL)
    run('iconutil','-c','icns',icons,'-o',resources/'MusicBoard.icns')
    info = {'CFBundleName':'MusicBoard','CFBundleDisplayName':'MusicBoard','CFBundleIdentifier':'center.dnd.MusicBoard','CFBundleExecutable':'MusicBoard','CFBundlePackageType':'APPL','CFBundleShortVersionString':VERSION,'CFBundleVersion':'1','CFBundleIconFile':'MusicBoard.icns','LSMinimumSystemVersion':'13.0','NSHighResolutionCapable':True,'NSHumanReadableCopyright':'2026 FullPeople. MIT. macOS Preview 1.'}
    (app/'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    licenses = resources/'Licenses'; licenses.mkdir()
    shutil.copy2(ROOT/'LICENSE', licenses/'MusicBoard-MIT.txt')
    shutil.copy2(source/'COPYING.LGPLv2.1', licenses/'FFmpeg-LGPLv2.1.txt')
    shutil.copy2(paths[0], licenses/'ffmpeg-9.0.2.tar.xz')
    shutil.copy2(ROOT/'tools/macos/build.py', licenses/'rebuild-media-runtime.py')
    for name in ['LICENSE','THIRD_PARTY_LICENSES.txt']:
        url = 'https://raw.githubusercontent.com/yt-dlp/yt-dlp/2026.08.19/'+name
        with urllib.request.urlopen(url, timeout=45) as response: (licenses/('yt-dlp-'+name)).write_bytes(response.read())
    record = {'ProductSource':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'Version':VERSION,'Architecture':platform.machine(),'Inputs':[{'Name':n,'URL':u,'SHA256':s} for n,u,s in SOURCES],'FFmpegConfigure':configure,'Signature':'ad-hoc only; no Developer ID or Apple notarization','Scope':'New native macOS preview. Windows release bytes untouched. Native checks and distribution checks are separate gates.'}
    (resources/'build-provenance.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    for name in ['ffmpeg','ffprobe','yt-dlp']: run('codesign','--force','--sign','-',tools/name)
    run('codesign','--force','--sign','-',app)
    run('codesign','--verify','--deep','--strict',app)
    (OUT/'build.json').write_text(json.dumps(record,indent=2),encoding='utf-8')

if __name__ == '__main__': main()
