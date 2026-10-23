"""Exercise safe ZIP staging against the real native extractor; never runs setup."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import struct
import subprocess
import uuid
import zipfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--archive', type=Path, required=True)
parser.add_argument('--probe', type=Path, required=True)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
archive_path, probe = args.archive.resolve(strict=True), args.probe.resolve(strict=True)
artifacts = (root / 'artifacts').resolve()
out = (args.output or artifacts / 'wizard-staging' / uuid.uuid4().hex).resolve()
if out == artifacts or not out.is_relative_to(artifacts) or out.exists():
    raise RuntimeError('Use a fresh output directory inside this checkout artifacts')
out.mkdir(parents=True)
checks, operations, measurements = {}, [], {}
protected = out / 'protected-user-data.txt'
protected.write_bytes(b'AUTHORED_PROTECTED_OUTSIDE_STAGING')
junction = None

def invoke(*arguments):
    result = subprocess.run([str(probe), *map(str, arguments)], capture_output=True,
                            text=True, encoding='utf-8', errors='replace', timeout=240)
    if not result.stdout.strip():
        raise RuntimeError('Native probe produced no JSON: ' + result.stderr[-2000:])
    data = json.loads(result.stdout)
    operations.append({'Arguments': list(map(str, arguments)), 'ExitCode': result.returncode,
                       'Error': data.get('Error', ''), 'Stderr': result.stderr[-2000:]})
    return result.returncode, data

def extract(package, destination, *options):
    return invoke(package, destination, '--cached-staging', *options)

def cleanup(destination, parent=out):
    return invoke('--cleanup', destination, parent)

def write_archive(path, entries):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as package:
        for name, data in entries:
            package.writestr(name, data)
        manifest = ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
                           for name, data in entries).encode('utf-8')
        package.writestr('SHA256SUMS.txt', manifest)

def monotonic(samples):
    return bool(samples) and all(0 <= done <= total for done, total in samples) and all(
        before[0] <= after[0] and before[1] == after[1] for before, after in zip(samples, samples[1:]))

report = {'Scope': 'Real native ZIP extraction and cleanup only, authored hostile fixtures and exact application payload; no setup, registry, application launch or UI control.'}
try:
    stage = out / 'FullTools-stage-actual'
    code, actual = extract(archive_path, stage)
    checks['ActualFlatPayloadExtracts'] = code == 0 and actual['Extracted']
    if not checks['ActualFlatPayloadExtracts']:
        raise RuntimeError('Cannot extract the actual payload: ' + actual['Error'])
    with zipfile.ZipFile(archive_path) as package:
        files = [entry for entry in package.infolist() if not entry.is_dir()]
        expected = {entry.filename for entry in files}
        checks['ActualPackageHasFlatExecutableContract'] = {'MusicBoard.exe', 'MusicBoardSetup.exe', 'VERSION.txt', 'SHA256SUMS.txt'}.issubset(expected)
        checks['ExactlyTheArchiveFilesAreStaged'] = expected == {
            path.relative_to(stage).as_posix() for path in stage.rglob('*') if path.is_file()}
        checks['EveryExtractedFileMatchesOriginalArchiveBytes'] = all(
            hashlib.sha256((stage / entry.filename).read_bytes()).digest() ==
            hashlib.sha256(package.read(entry)).digest() for entry in files)
        manifest = package.read('SHA256SUMS.txt')
        checks['OriginalManifestBytesAreUnchanged'] = (stage / 'SHA256SUMS.txt').read_bytes() == manifest
        checks['ManifestDigestMatchesAuthenticatedArchive'] = actual['ManifestSha256'] == hashlib.sha256(manifest).hexdigest()
        checks['ByteAndFileCountersMatchActualArchive'] = actual['ExtractedBytes'] == actual['TotalBytes'] == sum(entry.file_size for entry in files) and actual['Files'] == len(files)
        listed = {}
        for line in manifest.decode('utf-8-sig').splitlines():
            if line and not line.startswith('#'):
                digest, name = line.split('  ', 1)
                listed[name] = digest.lower()
        checks['OriginalManifestMatchesExactlyTheStagedFiles'] = set(listed) == expected - {'SHA256SUMS.txt'} and all(
            hashlib.sha256((stage / name).read_bytes()).hexdigest() == digest for name, digest in listed.items())
    samples = actual['ProgressSamples']
    checks['ActualByteProgressNeverMovesBackwards'] = monotonic(samples)
    checks['ActualProgressBeginsAtZeroAndEndsAtTotal'] = samples[0] == [0, actual['TotalBytes']] and samples[-1] == [actual['TotalBytes'], actual['TotalBytes']]
    measurements['ActualPayload'] = {key: value for key, value in actual.items() if key != 'ProgressSamples'}
    measurements['ActualProgressSamples'] = len(samples)
    measurements['ActualProgressFirstAndLast'] = [samples[0], samples[-1]]
    code, cleaned = cleanup(stage)
    checks['ActualStageCleanupSucceeds'] = code == 0 and cleaned['CleanupSucceeded'] and not stage.exists()

    fixture = out / 'fixture.zip'
    contents = b'AUTHORED_CANCELLATION_DATA\n' * 131072
    write_archive(fixture, [('MusicBoard.exe', b'AUTHORED_NOT_EXECUTABLE'), ('data/content.bin', contents)])

    traversal = out / 'traversal.zip'
    write_archive(traversal, [('folder/../../outside.bin', b'UNSAFE')])
    unsafe_stage = out / 'FullTools-stage-traversal'
    code, rejected = extract(traversal, unsafe_stage)
    checks['ArchivePathTraversalRejectedBeforeCreation'] = code == 1 and not unsafe_stage.exists() and not (out / 'outside.bin').exists()

    duplicate = out / 'duplicate.zip'
    write_archive(duplicate, [('Folder/Track.bin', b'FIRST'), ('folder/track.BIN', b'SECOND')])
    duplicate_stage = out / 'FullTools-stage-duplicate'
    code, rejected = extract(duplicate, duplicate_stage)
    checks['CaseInsensitiveDuplicatePathsRejected'] = code == 1 and 'duplicate' in rejected['Error'].lower() and not duplicate_stage.exists()

    corrupt = out / 'corrupt.zip'
    data = bytearray(fixture.read_bytes())
    with zipfile.ZipFile(fixture) as package:
        entry = package.getinfo('data/content.bin')
        name_bytes, extra_bytes = struct.unpack_from('<HH', data, entry.header_offset + 26)
        offset = entry.header_offset + 30 + name_bytes + extra_bytes
        data[offset + 123] ^= 1
    corrupt.write_bytes(data)
    corrupt_stage = out / 'FullTools-stage-corrupt'
    code, rejected = extract(corrupt, corrupt_stage)
    checks['DamagedFileCRCIsRejected'] = code == 1 and not rejected['Extracted'] and not rejected['Cancelled']
    code, cleaned = cleanup(corrupt_stage)
    checks['CorruptPartialStageCanBeCleaned'] = code == 0 and not corrupt_stage.exists()

    cancelled_stage = out / 'FullTools-stage-cancelled'
    code, cancelled = extract(fixture, cancelled_stage, '--cancel-after', 131072)
    checks['CancellationStopsBeforeAllBytesAreWritten'] = code == 1 and cancelled['Cancelled'] and not cancelled['Extracted'] and 0 < cancelled['ExtractedBytes'] < cancelled['TotalBytes']
    checks['CancelledProgressStillReportsRealMonotonicBytes'] = monotonic(cancelled['ProgressSamples']) and cancelled['ProgressSamples'][-1][0] == cancelled['ExtractedBytes']
    code, cleaned = cleanup(cancelled_stage)
    checks['CancelledPartialStageCanBeCleaned'] = code == 0 and not cancelled_stage.exists()

    unowned = out / 'not-an-owned-stage'
    unowned.mkdir(); (unowned / 'keep.txt').write_bytes(b'KEEP_UNOWNED')
    code, rejected = cleanup(unowned)
    checks['CleanupRefusesUnownedDirectory'] = code == 1 and (unowned / 'keep.txt').read_bytes() == b'KEEP_UNOWNED'

    owned = out / 'FullTools-stage-owned-but-wrong-parent'
    owned.mkdir(); (owned / 'keep.txt').write_bytes(b'KEEP_WRONG_PARENT')
    wrong_parent = out / 'unrelated-parent'; wrong_parent.mkdir()
    code, rejected = cleanup(owned, wrong_parent)
    checks['CleanupRefusesWrongExpectedParent'] = code == 1 and (owned / 'keep.txt').read_bytes() == b'KEEP_WRONG_PARENT'

    junction = out / 'linked-parent'
    target = out / 'junction-target'; target.mkdir()
    linked = subprocess.run([os.environ.get('COMSPEC', 'cmd.exe'), '/d', '/c', 'mklink', '/J', str(junction), str(target)],
                            capture_output=True, timeout=30)
    if linked.returncode != 0 or not junction.is_dir():
        raise RuntimeError('Cannot create an owned junction fixture: ' + linked.stderr.decode(errors='replace'))
    linked_stage = junction / 'FullTools-stage-linked'
    code, rejected = extract(fixture, linked_stage)
    checks['ReparseParentIsRejectedBeforeExtraction'] = code == 1 and 'linked' in rejected['Error'].lower() and not (target / linked_stage.name).exists()
    real_stage = target / linked_stage.name
    real_stage.mkdir(); (real_stage / 'keep.txt').write_bytes(b'KEEP_LINKED_TARGET')
    code, rejected = cleanup(linked_stage, junction)
    checks['CleanupRefusesReparseParentAndKeepsTarget'] = code == 1 and (real_stage / 'keep.txt').read_bytes() == b'KEEP_LINKED_TARGET'
    # rmdir removes this owned junction itself; it does not traverse its target.
    junction.rmdir(); junction = None

    checks['ProtectedOutsideDataRemainsUnchanged'] = protected.read_bytes() == b'AUTHORED_PROTECTED_OUTSIDE_STAGING'
    report['Passed'] = all(checks.values())
except Exception as error:
    report['Passed'] = False
    report['FatalError'] = str(error)
finally:
    if junction is not None and junction.exists():
        junction.rmdir()
    report.update({'Checks': checks, 'Operations': operations, 'Measurements': measurements})
    (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'Passed': report['Passed'], 'Checks': checks, 'Report': str(out / 'report.json'),
                      'FatalError': report.get('FatalError')}), flush=True)
if not report['Passed']:
    raise RuntimeError('Wizard staging verification failed: ' + str(out))
