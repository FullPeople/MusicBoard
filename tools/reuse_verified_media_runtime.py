"""Reuse only the exact published, source-matched runtime with complete provenance."""
from pathlib import Path
import hashlib,json,subprocess,sys,urllib.request,zipfile

ROOT=Path(__file__).resolve().parents[1]
EXPECTED='cd0f0f2d6949dbb0c26500a92c6002da9612c0bf620ea8152a648f77319c7c4c'
folder=ROOT/'.cache/verified-runtime-package';folder.mkdir(parents=True,exist_ok=True)
archive=folder/'MusicBoard-0.3.4-windows-x64.zip'
if not archive.exists():
    request=urllib.request.Request('https://github.com/FullPeople/MusicBoard/releases/download/v0.3.4/'+archive.name,headers={'Accept-Encoding':'identity'})
    with urllib.request.urlopen(request,timeout=60) as response,archive.open('xb') as output:
        if response.status!=200 or not response.geturl().startswith('https://'):raise RuntimeError('Invalid public runtime package response')
        count=0
        while data:=response.read(1048576):
            count+=len(data)
            if count>40972309:raise RuntimeError('Runtime package exceeds pinned size')
            output.write(data)
with archive.open('rb') as stream:
    if archive.stat().st_size!=40972309 or hashlib.file_digest(stream,'sha256').hexdigest()!=EXPECTED:raise RuntimeError('Exact public runtime archive differs')
out=ROOT/'.cache/media-tools';out.mkdir(parents=True,exist_ok=True)
prefix='MusicBoard-0.3.4-windows-x64/'
with zipfile.ZipFile(archive) as package:
    manifest={}
    for line in package.read(prefix+'SHA256SUMS.txt').decode('utf-8').splitlines():
        digest,name=line.split('  ',1)
        if name in manifest:raise RuntimeError('Duplicate original package record')
        manifest[name]=digest
    actual={info.filename for info in package.infolist() if not info.is_dir()}
    if actual!={prefix+name for name in manifest}|{prefix+'SHA256SUMS.txt'}:raise RuntimeError('Original package manifest is incomplete')
    for name,digest in manifest.items():
        if hashlib.sha256(package.read(prefix+name)).hexdigest()!=digest:raise RuntimeError('Original package payload differs: '+name)
    for name,digest in manifest.items():
        if not name.startswith('media-tools/') or name.startswith('media-tools/plugins/'):continue
        relative=Path(name[len('media-tools/'):])
        if relative.is_absolute() or '..' in relative.parts or '\\' in name:raise RuntimeError('Invalid runtime path')
        target=out/relative;target.parent.mkdir(parents=True,exist_ok=True)
        data=package.read(prefix+name)
        if target.exists() and target.read_bytes()!=data:raise RuntimeError('Existing runtime must be preserved: '+name)
        if not target.exists():target.write_bytes(data)
subprocess.run([sys.executable,str(ROOT/'tools/verify_media_runtime.py'),str(out)],check=True,timeout=120)
print(json.dumps({'Verified':True,'SourceMatchedPublishedArchive':EXPECTED,'RuntimeRecipe':'unchanged pinned FFmpeg/LAME and complete corresponding source; every original file and runtime provenance rechecked','Scope':'Runtime reuse only; fresh application/setup compilation, normal package smoke and actual playback remain required.'}))
