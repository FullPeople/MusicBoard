"""Verify publication/cancellation using real files on two explicit volumes."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid
import wave


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-base", type=Path, required=True)
    parser.add_argument("--target-base", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source_base = args.source_base.resolve(strict=True)
    owned = Path(tempfile.mkdtemp(prefix="musicboard-copy-", dir=source_base)).resolve()
    staging = owned / "staging"
    staging.mkdir()
    out = args.target_base.resolve(strict=True) / uuid.uuid4().hex
    out.mkdir()
    exe = root / "build/windows-x64/Release/import_copy_probe.exe"
    reports = []
    try:
        for mode in ("complete", "cancel"):
            source = staging / "copied.wav"
            with wave.open(str(source), "wb") as fixture:
                fixture.setnchannels(2)
                fixture.setsampwidth(2)
                fixture.setframerate(44100)
                block = bytes(range(256)) * 4096
                for _ in range(64):
                    fixture.writeframesraw(block)
            source_digest = digest(source)
            target = out / mode
            target.mkdir()
            sentinel = target / "copied.wav"
            sentinel.write_bytes(b"keep-existing-user-file")
            command = [str(exe), str(source), str(staging), str(target)]
            if mode == "cancel":
                command.append(str(8 * 1024 * 1024))
            result = subprocess.run(command, capture_output=True, timeout=30,
                                    encoding="utf-8", errors="replace")
            report = json.loads(result.stdout)
            report.update(Mode=mode, ExitCode=result.returncode,
                          ExistingFilePreserved=sentinel.read_bytes() == b"keep-existing-user-file")
            if mode == "complete":
                published = [path for path in target.glob("*.wav") if path != sentinel]
                report["CopiedHashMatches"] = len(published) == 1 and digest(published[0]) == source_digest
            else:
                report["SourceHashPreserved"] = source.exists() and digest(source) == source_digest
            report["Passed"] = report["Passed"] and report["ExistingFilePreserved"] and result.returncode == 0 and (
                report.get("CopiedHashMatches", report.get("SourceHashPreserved", False)))
            reports.append(report)
            print(json.dumps(report), flush=True)
        final = {"Passed": all(item["Passed"] for item in reports),
                 "SourceRoot": str(owned), "TargetRoot": str(out), "Results": reports}
        (out / "report.json").write_text(json.dumps(final, indent=2), encoding="utf-8")
        print("REPORT " + str(out / "report.json"), flush=True)
        if not final["Passed"]:
            raise RuntimeError("Actual cross-volume publication checks failed")
    finally:
        # Only this mkdtemp-owned directory is removed, after resolving its scope.
        if owned.parent != source_base or not owned.name.startswith("musicboard-copy-"):
            raise RuntimeError("Owned fixture path escaped its source base")
        if any(path.is_symlink() or path.is_junction() for path in owned.rglob("*")):
            raise RuntimeError("Fixture cleanup encountered a linked path")
        shutil.rmtree(owned)


if __name__ == "__main__":
    main()
