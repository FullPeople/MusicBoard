"""Run two real single-file installers in one owned root; never replace a user installation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import uuid
import winreg

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\MusicBoard"


def registered():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY) as key:
            return winreg.QueryValueEx(key, "DisplayVersion")[0]
    except FileNotFoundError:
        return None


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--from-version", default="0.3.1")
    parser.add_argument("--to-version", default="0.3.2")
    args = parser.parse_args()
    for version in (args.from_version, args.to_version):
        if not re.fullmatch(r"\d+\.\d+\.\d+", version):
            parser.error("Expected numeric product versions")
    shortcut = Path(os.environ["APPDATA"]) / "Microsoft/Windows/Start Menu/Programs/MusicBoard.lnk"
    if registered() is not None or shortcut.exists():
        raise RuntimeError("A user installation or shortcut exists; preserve it and stop this clean-registration probe")
    installers = [ROOT / "dist" / f"MusicBoard-{version}-Setup-windows-x64.exe" for version in (args.from_version, args.to_version)]
    if not all(path.is_file() for path in installers):
        raise RuntimeError("Both real single-file installers are required")
    output = ROOT / "artifacts/upgrade-tests" / uuid.uuid4().hex
    output.mkdir(parents=True)
    installed = output / "installed"
    profile = output / "profile"; library = output / "library"
    profile.mkdir(); library.mkdir()
    protected = [profile / "settings-marker.txt", library / "track-marker.txt"]
    for path in protected:
        path.write_text("owned preservation fixture", encoding="utf-8")
    before = [sha(path) for path in protected]
    checks = {}; failure = None
    try:
        first = subprocess.run([str(installers[0]), "--quiet", "--install-root", str(installed)], timeout=120, capture_output=True)
        checks["OlderSingleFileInstalled"] = first.returncode == 0 and registered() == args.from_version
        if not checks["OlderSingleFileInstalled"]:
            raise RuntimeError("Older installation failed")
        old = sha(installed / "current/MusicBoard.exe")
        second = subprocess.run([str(installers[1]), "--quiet", "--install-root", str(installed)], timeout=120, capture_output=True)
        checks["NewSingleFileUpgradedSameRoot"] = second.returncode == 0 and registered() == args.to_version
        main = installed / "current/MusicBoard.exe"
        version = subprocess.run([str(main), "--version"], timeout=15, capture_output=True)
        checks["NewProductVersionRuns"] = version.returncode == 0 and bool(re.search(
            r"(?<![\d.])" + re.escape(args.to_version) + r"(?![\d.])", version.stdout.decode("utf-8")))
        expected = ROOT / "dist" / f"MusicBoard-{args.to_version}-windows-x64/MusicBoard.exe"
        checks["InstalledMainMatchesReleasePayload"] = sha(main) == sha(expected) and sha(main) != old
        checks["NewUpdaterInstalled"] = (main.parent / "FullToolsUpdater.exe").is_file()
        checks["ExternalProfileAndLibraryFixturesPreservedAfterUpgrade"] = before == [sha(path) for path in protected]
    except Exception as error:
        failure = str(error)
    finally:
        uninstaller = installed / "current/MusicBoardSetup.exe"
        if uninstaller.exists():
            removed = subprocess.run([str(uninstaller), "--uninstall", "--quiet", "--install-root", str(installed)], timeout=45)
            deadline = time.monotonic() + 30
            while installed.exists() and time.monotonic() < deadline:
                time.sleep(.1)
            checks["UpgradedUninstallerRemovesInstallation"] = removed.returncode == 0 and not installed.exists()
        checks["RegistrationAndShortcutRemoved"] = registered() is None and not shortcut.exists()
        checks["ExternalFixturesPreservedAfterUninstall"] = before == [sha(path) for path in protected]
        report = {"Passed": failure is None and all(checks.values()), "Checks": checks, "Failure": failure,
                  "From": args.from_version, "To": args.to_version,
                  "Scope": "Real single-file cross-version installation and uninstall; external generated data fixtures; no online updater transport claim"}
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps({**report, "Evidence": str(output)}), flush=True)
    return 0 if report["Passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
