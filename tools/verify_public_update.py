"""Actual published HTTPS feed -> updater window -> install -> uninstall in an owned root."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid
import winreg
import psutil
import pywintypes
import win32con
import win32gui
import win32process

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\MusicBoard'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def registered():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY) as key:
            return winreg.QueryValueEx(key, 'DisplayVersion')[0]
    except FileNotFoundError:
        return None


def wait(condition, seconds, description):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(.1)
    raise TimeoutError(description)


def main():
    shortcut = Path(os.environ['APPDATA']) / 'Microsoft/Windows/Start Menu/Programs/MusicBoard.lnk'
    if registered() is not None or shortcut.exists():
        raise RuntimeError('Preserve existing user installation/shortcut; clean QA cannot start')
    if any(item.info['name'] and item.info['name'].lower() == 'musicboard.exe' for item in psutil.process_iter(['name'])):
        raise RuntimeError('Preserve any running MusicBoard instance; clean QA cannot start')
    output = Path(os.environ['LOCALAPPDATA']) / 'FullTools-QA' / ('update-' + uuid.uuid4().hex)
    output.mkdir(parents=True)
    installed = output / 'installed'
    temporary = output / 'temporary'; temporary.mkdir()
    profile = output / 'profile'; profile.mkdir()
    library = output / 'library'
    for part in ('BGM', 'SFX'):
        (library / part).mkdir(parents=True)
    protected = [profile / 'preserve-settings.txt', library / 'BGM/preserve-library.txt']
    for path in protected:
        path.write_text('owned user data preservation fixture', encoding='utf-8')
    before = [sha(path) for path in protected]
    env = dict(os.environ, LOCALAPPDATA=str(profile), TEMP=str(temporary), TMP=str(temporary))
    checks = {}; error = None; process = None; panel = None; texts = []; installer_result = None; settings = {}

    def updater_windows():
        found = []
        def each(hwnd, _):
            try:
                if win32gui.GetClassName(hwnd) != 'FullTools.Update':
                    return
                pid = win32process.GetWindowThreadProcessId(hwnd)[1]
                arguments = psutil.Process(pid).cmdline()
                if str(installed) in arguments:
                    found.append(hwnd)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
            except pywintypes.error as failure:
                if failure.winerror != 1400:
                    raise
        win32gui.EnumWindows(each, None)
        return found

    def close_updater(hwnd):
        try:
            win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        except pywintypes.error as failure:
            if failure.winerror != 1400:
                raise

    def labels(hwnd):
        values = []
        win32gui.EnumChildWindows(hwnd, lambda child, _: values.append(win32gui.GetWindowText(child)), None)
        return ' '.join(values)

    def status_contains(hwnd, expected):
        text = labels(hwnd)
        if '更新检查未完成' in text or '下载未完成' in text or '更新未完成' in text:
            raise RuntimeError(text)
        if expected in text:
            texts.append(text)
            return text
        return None

    try:
        old = ROOT / 'dist/MusicBoard-0.3.1-Setup-windows-x64.exe'
        result = subprocess.run([str(old), '--quiet', '--install-root', str(installed)],
                                env=env, timeout=120, capture_output=True)
        installer_result = {'ExitCode': result.returncode, 'Stdout': result.stdout.decode('utf-8', errors='replace'),
                            'Stderr': result.stderr.decode('utf-8', errors='replace')}
        checks['InstalledRealOlderVersion'] = result.returncode == 0 and registered() == '0.3.1'
        if not checks['InstalledRealOlderVersion']:
            raise RuntimeError('Older installer failed')
        main_exe = installed / 'current/MusicBoard.exe'
        old_hash = sha(main_exe)
        process = subprocess.Popen([str(main_exe), '--library', str(library)], env=env,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        def own_panel():
            candidates = []
            win32gui.EnumWindows(lambda h, _: candidates.append(h) if win32process.GetWindowThreadProcessId(h)[1] == process.pid
                and win32gui.GetClassName(h) == 'MusicBoard.CompositionWindow' else None, None)
            if process.poll() is not None:
                raise RuntimeError('Older installed app exited before update')
            return candidates[0] if candidates else None
        panel = wait(own_panel, 20, 'Older installed application panel')
        checks['ActualOlderApplicationRuns'] = True
        # The real application starts its signed background update check at startup.
        hwnd = wait(lambda: updater_windows()[0] if updater_windows() else None, 60, 'Published background update window')
        wait(lambda: status_contains(hwnd, '发现新版本 0.3.2'), 45, 'Signed public feed result')
        checks['RealApplicationDiscoversPublishedSignedUpdate'] = True
        win32gui.PostMessage(hwnd, win32con.WM_COMMAND, 1, 0)
        wait(lambda: status_contains(hwnd, '下载和校验完成'), 600, 'HTTPS download and SHA verification')
        downloads = list(temporary.glob('FullTools-update-*/update-installer.exe'))
        checks['DownloadedInstallerMatchesPublicRelease'] = len(downloads) == 1 and sha(downloads[0]) == '723f3516705e7abf23bec55d85638886692c9bde39c88efa29874c87d4865a4e'
        checks['DownloadDoesNotReplaceRunningApplication'] = sha(main_exe) == old_hash and process.poll() is None
        win32gui.PostMessage(hwnd, win32con.WM_COMMAND, 1, 0)
        wait(lambda: status_contains(hwnd, '软件仍在运行'), 10, 'Running app protection')
        checks['InstallationWaitsForActualApplicationExit'] = sha(main_exe) == old_hash and registered() == '0.3.1'
        win32gui.PostMessage(panel, win32con.WM_COMMAND, 3, 0)
        process.wait(timeout=15)
        checks['OlderApplicationExitsNormally'] = process.returncode == 0
        settings = {p.relative_to(profile).as_posix(): sha(p) for p in profile.rglob('settings.conf')}
        checks['ActualApplicationSettingsCreated'] = bool(settings)
        win32gui.PostMessage(hwnd, win32con.WM_COMMAND, 1, 0)
        wait(lambda: status_contains(hwnd, '更新完成。'), 120, 'Downloaded public installer upgrade')
        checks['PublicUpdaterInstallsNewVersion'] = registered() == '0.3.2' and sha(main_exe) == sha(ROOT / 'dist/MusicBoard-0.3.2-windows-x64/MusicBoard.exe')
        version = subprocess.run([str(main_exe), '--version'], env=env, timeout=15, capture_output=True)
        checks['UpdatedApplicationReports032'] = version.returncode == 0 and b'0.3.2' in version.stdout
        checks['RealSettingsAndExternalLibraryPreserved'] = before == [sha(p) for p in protected] and all(sha(profile / p) == h for p, h in settings.items())
        close_updater(hwnd)
        wait(lambda: not updater_windows() and not list(temporary.glob('FullTools-update-*')), 25, 'Update worker cleanup')
        checks['DownloadedWorkerCleansAfterSuccess'] = True
        launcher = subprocess.run([str(installed / 'current/FullToolsUpdater.exe'), '--product', 'MusicBoard', '--version', '0.3.2'],
                                  env=env, timeout=15)
        hwnd = wait(lambda: updater_windows()[0] if updater_windows() else None, 20, 'Current-version check window')
        wait(lambda: status_contains(hwnd, '当前版本已是最新版本'), 45, 'Latest-version result')
        checks['UpdatedVersionRecognizedAsLatest'] = launcher.returncode == 0
        close_updater(hwnd)
    except Exception as failure:
        error = str(failure)
    finally:
        if process and process.poll() is None and panel:
            win32gui.PostMessage(panel, win32con.WM_COMMAND, 3, 0)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                error = (error or '') + '; owned application did not exit normally'
        for hwnd in updater_windows():
            close_updater(hwnd)
        try:
            wait(lambda: not updater_windows() and not list(temporary.glob('FullTools-update-*')), 35, 'Final updater cleanup')
            checks['NoOwnedUpdateWorkerLeft'] = True
            uninstaller = installed / 'current/MusicBoardSetup.exe'
            if uninstaller.is_file() and (not process or process.poll() is not None):
                removed = subprocess.run([str(uninstaller), '--uninstall', '--quiet', '--install-root', str(installed)], env=env, timeout=45)
                wait(lambda: not installed.exists(), 35, 'Installed uninstaller')
                checks['NormalUninstallRemovesOnlyOwnedInstallation'] = removed.returncode == 0 and registered() is None and not shortcut.exists()
            checks['ExternalDataStillPreservedAfterUninstall'] = before == [sha(p) for p in protected]
            checks['ActualSettingsPreservedAfterUninstall'] = bool(settings) and all(sha(profile / p) == h for p, h in settings.items())
        except Exception as failure:
            error = (error or '') + '; cleanup: ' + str(failure)
        report = {'Passed': error is None and all(checks.values()), 'Checks': checks, 'Failure': error,
                  'VisibleResults': texts, 'Evidence': str(output), 'OlderInstallerResult': installer_result,
                  'Scope': 'Actual old native app, public signed HTTPS discovery/download, running-app guard, same-root install, version and uninstall; own generated fixtures only'}
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(report, ensure_ascii=False), flush=True)
    return 0 if report['Passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
