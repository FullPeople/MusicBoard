"""Build an authenticated, flat MusicBoard package from a verified public runtime."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, zipfile

ROOT=Path(__file__).resolve().parents[1]
BASE_SHA256='754ce29d758dc757712fa70e1684dfa1c1f1f42fd9d90ce8664debfeb280bf21'

def sha(data):return hashlib.sha256(data).hexdigest()

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--base',type=Path,required=True)
    parser.add_argument('--build-directory',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--version',default='0.3.6')
    args=parser.parse_args()
    if args.output.exists():raise RuntimeError('Preserve the existing package; choose a new output')
    if not args.output.resolve().is_relative_to((ROOT/'artifacts').resolve()):raise RuntimeError('Output must stay in owned artifacts')
    with args.base.open('rb') as f:
        if hashlib.file_digest(f,'sha256').hexdigest()!=BASE_SHA256:raise RuntimeError('Published runtime archive differs')
    with zipfile.ZipFile(args.base) as archive:
        data={}
        for item in archive.infolist():
            if item.is_dir():continue
            p=PurePosixPath(item.filename)
            if p.is_absolute() or '..' in p.parts or '\\' in item.filename:raise RuntimeError('Unsafe source archive path')
            if len(p.parts)<2 or p.parts[0]!='MusicBoard-0.3.5-windows-x64':raise RuntimeError('Unexpected public package layout')
            name=PurePosixPath(*p.parts[1:]).as_posix()
            if name in data:raise RuntimeError('Duplicate source archive entry')
            data[name]=archive.read(item)
    manifest=data.pop('SHA256SUMS.txt').decode('utf-8-sig')
    listed={}
    for line in manifest.splitlines():
        digest,name=line.split('  ',1)
        if name in listed or name not in data or sha(data[name])!=digest:raise RuntimeError('Original file manifest differs: '+name)
        listed[name]=digest
    if set(listed)!=set(data):raise RuntimeError('Unlisted public package files')
    original_updater_sha=sha(data['FullToolsUpdater.exe'])
    for name in ['MusicBoard.exe','MusicBoardSetup.exe']:
        source=args.build_directory/name
        if name=='MusicBoard.exe' and not source.exists():source=args.build_directory/'musicboard.exe'
        data[name]=source.read_bytes()
    version=args.version
    data['VERSION.txt']=('\n'.join(['product=MusicBoard','version='+version,'package_version='+version+'-wizard.1','architecture=windows-x64','executable=MusicBoard.exe','complete=1'])+'\n').encode()
    data['INSTALLER-REVISION.txt']=b'wizard.1\n'
    for name,source in [('README.zh-CN.txt',ROOT/'packaging/README.zh-CN.txt'),('README.en.txt',ROOT/'packaging/README.en.txt'),('THIRD_PARTY_NOTICES.txt',ROOT/'packaging/THIRD_PARTY_NOTICES.txt')]:
        data[name]=source.read_bytes()
    miniz_license=ROOT/'src/distribution/third_party/miniz/LICENSE'
    if not miniz_license.is_file():raise RuntimeError('Installer miniz license must be included')
    data['LICENSES/miniz-LICENSE.txt']=miniz_license.read_bytes()
    data['LICENSES/FullTools-Installer-MIT.txt']=(ROOT/'src/distribution/FULLTOOLS-LICENSE.txt').read_bytes()
    digests={name:sha(content) for name,content in data.items()}
    data['SHA256SUMS.txt']=''.join(digests[name]+'  '+name+'\n' for name in sorted(data)).encode('utf-8')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.output,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for name in sorted(data):archive.writestr(name,data[name])
    report={'version':version,'installerRevision':'wizard.1','baseArchiveSHA256':BASE_SHA256,'originalManifestVerified':True,'files':len(data),'unpackedBytes':sum(map(len,data.values())),'updaterReusedUnchanged':True,'updaterSHA256':original_updater_sha,'applicationSHA256':digests['MusicBoard.exe'],'setupSHA256':digests['MusicBoardSetup.exe'],'manifestSHA256':sha(data['SHA256SUMS.txt'])}
    args.output.with_suffix('.provenance.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))

if __name__=='__main__':main()
