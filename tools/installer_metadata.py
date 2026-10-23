"""Generate wizard sizes and license resources from the exact packaged payload."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('archive', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)

with zipfile.ZipFile(args.archive) as package:
    files = [entry for entry in package.infolist() if not entry.is_dir()]
    names = [entry.filename for entry in files]
    if len(names) != len({name.casefold() for name in names}):
        raise ValueError('Payload contains duplicate archive paths')
    values = dict(line.split('=', 1) for line in package.read('VERSION.txt').decode('utf-8-sig').splitlines() if '=' in line)
    for key, expected in {'product': 'MusicBoard', 'architecture': 'windows-x64', 'executable': 'MusicBoard.exe', 'complete': '1'}.items():
        if values.get(key) != expected:
            raise ValueError('Invalid MusicBoard version contract: ' + key)
    version = values['version']
    if not version or any(character not in '0123456789.-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ' for character in version):
        raise ValueError('Invalid version literal')
    main_license = 'LICENSE.txt' if 'LICENSE.txt' in names else 'LICENSE'
    mit = package.read(main_license).decode('utf-8-sig')
    notices = package.read('THIRD_PARTY_NOTICES.txt').decode('utf-8-sig')
    media_licenses = []
    seen = set()
    for entry in sorted(files, key=lambda item: item.filename):
        relative = PurePosixPath(entry.filename)
        lowered = entry.filename.lower()
        is_license = ('/licenses/' in lowered or lowered.startswith('licenses/'))
        if not is_license or relative.suffix.lower() not in ('', '.txt', '.md'):
            continue
        data = package.read(entry)
        if len(data) > 4 * 1024 * 1024:
            raise ValueError('License text is unexpectedly large: ' + entry.filename)
        digest = hashlib.sha256(data).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        media_licenses.append('\n\n' + entry.filename + '\n' + '=' * 64 + '\n' + data.decode('utf-8-sig'))
    if not media_licenses:
        raise ValueError('Payload is missing bundled third-party license texts')

for name, content in [('MIT', mit), ('Notices', notices), ('Media', ''.join(media_licenses))]:
    if '\x00' in content:
        raise ValueError('License contains an embedded null')
    (args.output.parent / ('Wizard-' + name + '.txt')).write_text(content, encoding='utf-8')

header = '#pragma once\n#include <cstdint>\nnamespace setup_metadata {\n'
header += 'inline constexpr wchar_t version[] = L"' + version + '";\n'
header += 'inline constexpr std::uint64_t total_bytes = ' + str(sum(entry.file_size for entry in files)) + 'ULL;\n'
header += '}\n'
args.output.write_text(header, encoding='utf-8')
print(json.dumps({'Product': 'MusicBoard', 'Version': version, 'Files': len(files),
                  'Bytes': sum(entry.file_size for entry in files), 'LicenseDocuments': len(media_licenses) + 2}))
