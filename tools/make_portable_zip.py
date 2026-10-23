from pathlib import Path
import argparse, zipfile

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--payload',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--version',default='0.3.6')
args=parser.parse_args()
if not args.output.resolve().is_relative_to((root/'dist').resolve()):raise RuntimeError('Portable output must stay in owned dist')
args.output.parent.mkdir(parents=True,exist_ok=True)
temporary=args.output.with_name(args.output.name+'.pending')
with zipfile.ZipFile(args.payload) as source,zipfile.ZipFile(temporary,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as dest:
    for item in source.infolist():
        if not item.is_dir():dest.writestr('MusicBoard-'+args.version+'-windows-x64/'+item.filename,source.read(item))
temporary.replace(args.output)
print(str(args.output))
