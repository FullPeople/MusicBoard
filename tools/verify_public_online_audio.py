"""Verify public links through the real native dialog and XAudio2 playback."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

from public_link_import_probe import CASES

EXPECTED = (
    "online mode disables download autoplay option",
    "native import button starts online task",
    "native online input publishes prepared online entry",
    "application playback crosses the streaming head",
    "application stop ends online playback",
    "online entry has no downloaded audio file",
)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--case", choices=CASES, action="append", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    out = args.output_root.resolve() / uuid.uuid4().hex
    out.mkdir(parents=True, exist_ok=False)
    results = []
    for name in args.case:
        case = out / name
        library = case / "library"
        for kind in ("BGM", "SFX"):
            (library / kind).mkdir(parents=True, exist_ok=False)
        print(json.dumps({"Starting": name, "Mode": "native online playback"}), flush=True)
        error = None
        with (case / "native.log").open("wb") as log:
            process = subprocess.Popen([str(exe), "--library", str(library), "--selftest-online", CASES[name]],
                env=dict(os.environ, LOCALAPPDATA=str(case / "profile")),
                stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                code = process.wait(timeout=100)
            except subprocess.TimeoutExpired:
                error = "Owned native online test reached its deadline"
                if process.poll() is None:
                    process.terminate()
                code = process.wait(timeout=10)
        lines = (case / "native.log").read_text(encoding="utf-8", errors="replace").splitlines()
        checks = {label: "PASS " + label in lines for label in EXPECTED}
        checks["NoAudioDeviceCheckSkipped"] = not any(line.startswith("SKIP ") for line in lines)
        checks["OwnedProcessExitedNormally"] = code == 0
        checks["NoDownloadedLibraryFiles"] = not any(path.is_file() for path in library.rglob("*"))
        result = {"Provider": name, "Passed": error is None and all(checks.values()),
            "Checks": checks, "ExitCode": code, "OwnedPid": process.pid, "Failure": error}
        results.append(result)
        report = {"Passed": len(results) == len(args.case) and all(r["Passed"] for r in results),
            "ExecutableSHA256": hashlib.sha256(exe.read_bytes()).hexdigest(), "Results": results,
            "Scope": "Actual public links, native import button, prepared online entry, XAudio2 position beyond the two-second head, stop and no audio-library writes; finite samples, not all songs or long-outage acceptance"}
        (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps(result), flush=True)
    print(json.dumps({"Evidence": str(out / "report.json"), "Passed": report["Passed"]}), flush=True)
    return 0 if report["Passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
