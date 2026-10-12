"""Exercise real Win32 window messages, screenshots, first launch, and normal exit.

Each process uses an isolated LOCALAPPDATA directory. The real audio library is
read only; this script never installs or changes the user's settings.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import re
import subprocess
import time

from PIL import ImageGrab

u = c.WinDLL('user32', use_last_error=True)
callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [callback_type, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.GetDpiForWindow.argtypes = [w.HWND]
u.GetDpiForWindow.restype = w.UINT
u.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SendMessageW.restype = c.c_ssize_t
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
u.SetForegroundWindow.argtypes = [w.HWND]
u.IsWindowVisible.argtypes = [w.HWND]
u.mouse_event.argtypes = [w.DWORD, w.DWORD, w.DWORD, w.DWORD, c.c_size_t]
u.SetProcessDpiAwarenessContext.argtypes = [c.c_void_p]
u.SetProcessDpiAwarenessContext(c.c_void_p(-4))

def find_window(pid, name):
    matches = []
    @callback_type
    def visit(hwnd, unused):
        owner = w.DWORD()
        u.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid:
            text = c.create_unicode_buffer(256)
            u.GetClassNameW(hwnd, text, 256)
            if text.value == name: matches.append(hwnd)
        return True
    u.EnumWindows(visit, 0)
    return matches[0] if matches else None

def until(condition, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = condition()
        if result: return result
        time.sleep(0.03)
    raise AssertionError('timed out waiting for native product state')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--library', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    exe = str(Path(args.exe).resolve())
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    checks = []
    def check(value, label):
        checks.append({'pass': bool(value), 'label': label})
        print(('PASS ' if value else 'FAIL ') + label, flush=True)
        if not value: raise AssertionError(label)
    env = dict(os.environ, LOCALAPPDATA=str(out / 'profile'), MUSICBOARD_TRACE_INPUT='1')
    Path(env['LOCALAPPDATA']).mkdir(exist_ok=True)
    with (out / 'stdout.txt').open('wb') as stdout, (out / 'stderr.txt').open('wb') as stderr:
        proc = subprocess.Popen([exe, '--library', args.library], env=env, stdout=stdout, stderr=stderr)
        started = time.monotonic()
        panel = None
        try:
            panel = until(lambda: find_window(proc.pid, 'MusicBoard.CompositionWindow'))
            orb = until(lambda: find_window(proc.pid, 'MusicBoard.OrbWindow'))
            until(lambda: u.IsWindowVisible(panel))
            print(f'window_ready_seconds={time.monotonic() - started:.3f}', flush=True)
            u.SetWindowPos(panel, None, 80, 40, 0, 0, 0x15)
            u.SetForegroundWindow(panel)
            time.sleep(1.0)
            rect = w.RECT()
            u.GetWindowRect(panel, c.byref(rect))
            scale = u.GetDpiForWindow(panel) / 96
            check(rect.right > rect.left and rect.bottom > rect.top, 'panel located by PID and class, with actual DPI')
            def log():
                return (out / 'stdout.txt').read_text(encoding='utf-8', errors='replace')
            def pack(x, y): return (round(y * scale) & 0xffff) << 16 | (round(x * scale) & 0xffff)
            def hit(x, y):
                u.SendMessageW(panel, 0x200, 0, pack(x, y))
                ids = re.findall(r' id=(-?\d+)', log())
                return int(ids[-1]) if ids else -1
            def move(x, y):
                u.SendMessageW(panel, 0x200, 0, pack(x, y))
            def click(x, y):
                move(x, y)
                time.sleep(0.05)
                u.SendMessageW(panel, 0x201, 1, pack(x, y))
                time.sleep(0.04)
                u.SendMessageW(panel, 0x202, 0, pack(x, y))
                time.sleep(0.15)
            def screenshot(name):
                # Capturing the displayed desktop avoids PrintWindow's DComp race.
                image = ImageGrab.grab(bbox=(rect.left, rect.top, rect.right, rect.bottom), all_screens=True)
                image.save(out / name)
            def drag(point, destination):
                move(*point)
                time.sleep(0.1)
                u.SendMessageW(panel, 0x201, 1, pack(*point))
                for step in range(1, 13):
                    move(point[0] + (destination[0] - point[0]) * step / 12,
                         point[1] + (destination[1] - point[1]) * step / 12)
                    time.sleep(0.02)
                u.SendMessageW(panel, 0x202, 0, pack(*destination))
            rows = {}
            for y in range(410, 736, 6):
                target = hit(100, y)
                if 1000 <= target < 2000 and target not in rows: rows[target] = (100, y + 5)
                if len(rows) >= 2: break
            check(len(rows) >= 2, 'two actual BGM card rows discovered from hit testing')
            screenshot('01-idle.png')
            first, second = list(rows.values())[:2]
            before = len(log())
            drag(first, (110, 180))
            until(lambda: 'status: 正在播放' in log()[before:])
            check(True, 'Win32 mouse messages start the requested BGM')
            before = len(log())
            click(110, 180)
            until(lambda: 'status: 已暂停' in log()[before:])
            check(True, 'real disc click pauses playback')
            before = len(log())
            click(110, 180)
            until(lambda: 'status: 继续' in log()[before:])
            check(True, 'second disc click resumes playback')
            before = len(log())
            drag(second, (310, 180))
            until(lambda: 'status: 正在播放' in log()[before:])
            check(True, 'drop anywhere on the deck switches tracks')
            screenshot('02-playing.png')
            clear = next(((x, 66) for x in range(60, 225, 3) if hit(x, 66) == 130), None)
            check(clear is not None, 'clear button is hit-testable')
            before = len(log())
            click(*clear)
            until(lambda: 'status: BGM 已清空' in log()[before:])
            check(True, 'real clear click clears BGM')
            time.sleep(0.5)
            screenshot('03-cleared.png')
            # Validate title-bar buttons through WM_NCHITTEST before clicking.
            title_x = rect.right - round(96 * scale)
            title_y = rect.top + round(20 * scale)
            nc = u.SendMessageW(panel, 0x84, 0, ((title_y & 0xffff) << 16) | (title_x & 0xffff))
            check(nc == 1, 'title-bar button returns HTCLIENT')
            click((rect.right - rect.left) / scale - 96, 20)
            until(lambda: not u.IsWindowVisible(panel), 3)
            check(u.IsWindowVisible(orb), 'collapsing panel keeps the orb visible')
            u.SendMessageW(orb, 0x201, 1, (92 << 16) | 92)
            u.SendMessageW(orb, 0x202, 0, (92 << 16) | 92)
            until(lambda: u.IsWindowVisible(panel), 3)
            check(True, 'orb click restores the panel through its window procedure')
            u.SendMessageW(panel, 0x111, 3, 0)
            check(proc.wait(timeout=10) == 0, 'tray Exit closes normally without native crash')
        finally:
            if proc.poll() is None:
                if panel: u.SendMessageW(panel, 0x111, 3, 0)
                try: proc.wait(timeout=10)
                except subprocess.TimeoutExpired: proc.terminate()
            (out / 'checks.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')

    # First launch must show a folder picker; cancelling is a normal exit.
    first_env = dict(env, LOCALAPPDATA=str(out / 'first-launch'))
    Path(first_env['LOCALAPPDATA']).mkdir(exist_ok=True)
    with (out / 'first-launch.txt').open('wb') as output:
        proc = subprocess.Popen([exe], env=first_env, stdout=output, stderr=output)
        try:
            dialog = until(lambda: find_window(proc.pid, '#32770'), 10)
            check(bool(dialog), 'first launch displays a native library folder picker')
            u.SendMessageW(dialog, 0x10, 0, 0)
            check(proc.wait(timeout=10) == 0, 'cancelling first-launch folder selection exits normally')
        finally:
            if proc.poll() is None: proc.terminate()
    (out / 'checks.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    print(f'{len(checks)} checks passed', flush=True)

if __name__ == '__main__': main()
