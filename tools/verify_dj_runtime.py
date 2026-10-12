"""Current desktop verification, using real window messages and isolated settings."""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import re
import subprocess
import time
from PIL import ImageGrab, ImageStat
from verify_runtime import find_window, until, u, w

u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]

class ProcessMemory(c.Structure):
    _fields_ = [('cb',w.DWORD),('faults',w.DWORD)] + [(name,c.c_size_t) for name in
        ('peak_working','working','peak_paged','paged','peak_nonpaged','nonpaged','private','peak_private')]

def report_memory(proc):
    counters=ProcessMemory(); counters.cb=c.sizeof(counters)
    get_memory=c.WinDLL('psapi',use_last_error=True).GetProcessMemoryInfo
    get_memory.argtypes=[w.HANDLE,c.POINTER(ProcessMemory),w.DWORD]
    if not get_memory(w.HANDLE(int(proc._handle)),c.byref(counters),counters.cb):
        raise c.WinError(c.get_last_error())
    print(f'PROCESS_MEMORY working_mb={counters.working/1048576:.2f} peak_working_mb={counters.peak_working/1048576:.2f} private_mb={counters.private/1048576:.2f}',flush=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--library', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--profile')
    parser.add_argument('--startup-only', action='store_true')
    args = parser.parse_args()
    out = Path(args.out).resolve(); out.mkdir(parents=True, exist_ok=True)
    profile = Path(args.profile).resolve() if args.profile else out / 'profile'
    profile.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, LOCALAPPDATA=str(profile), MUSICBOARD_TRACE_INPUT='1')
    checks = []
    def check(value, label):
        checks.append(dict(pass_=bool(value), label=label))
        print(('PASS ' if value else 'FAIL ') + label, flush=True)
        if not value: raise AssertionError(label)
    def logfile():
        logs = sorted((profile / 'MusicBoard' / 'logs').glob('app-*.log'))
        return ''.join(p.read_text(encoding='utf-8', errors='replace') for p in logs)
    previous_log = len(logfile())
    with (out / 'stdout.txt').open('wb') as stdout, (out / 'stderr.txt').open('wb') as stderr:
        started = time.monotonic()
        proc = subprocess.Popen([str(Path(args.exe).resolve()), '--library', args.library], env=env, stdout=stdout, stderr=stderr)
        panel = None
        try:
            panel = until(lambda: find_window(proc.pid, 'MusicBoard.CompositionWindow'), 12)
            orb = until(lambda: find_window(proc.pid, 'MusicBoard.OrbWindow'), 4)
            rect = w.RECT(); u.GetWindowRect(panel, c.byref(rect))
            scale = u.GetDpiForWindow(panel) / 96.0
            def log(): return logfile()[previous_log:]
            def packed(x, y): return (round(y * scale) & 65535) << 16 | (round(x * scale) & 65535)
            def send(msg, x, y, wp=0): return u.SendMessageW(panel, msg, wp, packed(x, y))
            def hit(x, y):
                send(0x200, x, y)
                ids = re.findall(r' id=(-?\d+)', log())
                return int(ids[-1]) if ids else -1
            def click(x, y):
                send(0x200, x, y); send(0x201, x, y, 1); send(0x202, x, y); time.sleep(.08)
            def screenshot(name, hwnd=panel):
                bounds = w.RECT(); u.GetWindowRect(hwnd, c.byref(bounds))
                picture = ImageGrab.grab(bbox=(bounds.left,bounds.top,bounds.right,bounds.bottom), all_screens=True)
                picture.save(out / name); return picture
            if u.IsWindowVisible(panel): screenshot('01-loading.png')
            until(lambda: 'PRELOAD_READY' in log() or proc.poll() is not None, 90)
            check(proc.poll() is None, 'process survives library preparation')
            ready = re.findall(r'PRELOAD_READY[^\r\n]+', log())[-1]
            print(ready + f' wall_seconds={time.monotonic()-started:.3f}', flush=True)
            report_memory(proc)
            until(lambda: not u.IsWindowVisible(panel), 3)
            check(u.IsWindowVisible(orb), 'after loading only the always-visible orb remains')
            if args.startup_only:
                u.SendMessageW(panel,0x111,3,0)
                check(proc.wait(timeout=10)==0,'normal exit returns zero')
                return
            def orb_click():
                u.SendMessageW(orb, 0x201, 1, (92 << 16) | 92)
                u.SendMessageW(orb, 0x202, 0, (92 << 16) | 92)
                time.sleep(.5)
            orb_click(); check(u.IsWindowVisible(panel), 'orb opens the panel')
            u.SetWindowPos(panel, None, 40, 40, 0, 0, 0x15)
            u.SetWindowPos(orb, c.c_void_p(-1), 1340, 120, 0, 0, 0x15)
            mode = next(((x,y) for y in range(85,124,4) for x in range(850,950,8) if hit(x,y)==7), None)
            check(mode is not None, 'DJ mode selector has a native hit region')
            click(mode[0]-90,mode[1])  # Establish turntable mode even with a reused DJ profile.
            click(*mode)
            check('DJ 按钮 ·' in log(), 'selector switches to DJ button mode')
            rows = {}
            for y in range(180,730,5):
                identity=hit(840,y)
                if 2000 <= identity < 3000 and identity not in rows: rows[identity]=(840,y+12)
                if len(rows)>=2: break
            check(len(rows)>=2, 'DJ pads have separate usable hit regions')
            for xy in list(rows.values())[:2]: click(*xy)
            check(len(re.findall(r'PAD_START', log()))>=2, 'two different buttons submit independent voices')
            screenshot('02-dj-playing.png')
            # Start a BGM through its actual card/deck drag path.
            bgm = next(((100,y+7) for y in range(410,730,5) if 1000<=hit(100,y)<2000), None)
            check(bgm is not None, 'BGM card found from current layout')
            send(0x200,*bgm); send(0x201,*bgm,1)
            send(0x200,115,190,1); send(0x202,115,190)
            until(lambda: 'status: 正在播放' in log(), 4)
            # Playback status is emitted before the next UI frame has installed
            # the cover. Establish the playing visual before comparing frames.
            def playing_orb_visible():
                bounds=w.RECT();u.GetWindowRect(orb,c.byref(bounds))
                pic=ImageGrab.grab(bbox=(bounds.left,bounds.top,bounds.right,bounds.bottom),all_screens=True)
                colors=ImageStat.Stat(pic.crop((pic.width//2-3,pic.height//2-3,pic.width//2+3,pic.height//2+3))).mean
                return colors[0]-colors[2]>40
            until(playing_orb_visible, 4)
            # Capture the displayed orb repeatedly; center color must never vanish or drift.
            orb_samples=[]
            for i in range(48):
                bounds=w.RECT();u.GetWindowRect(orb,c.byref(bounds))
                pic=ImageGrab.grab(bbox=(bounds.left,bounds.top,bounds.right,bounds.bottom),all_screens=True)
                center=pic.crop((pic.width//2-3,pic.height//2-3,pic.width//2+3,pic.height//2+3))
                orb_samples.append(ImageStat.Stat(center).mean)
                if i in (0,12,24,47): pic.save(out/f'orb-{i:02d}.png')
                time.sleep(.04)
            red=[v[0] for v in orb_samples]
            check(min(red)>30 and max(red)-min(red)<65, f'48 playing-orb captures keep a stable visible center range={min(red):.1f}..{max(red):.1f}')
            orb_click(); check(not u.IsWindowVisible(panel) and u.IsWindowVisible(orb), 'orb closes panel while playback continues')
            orb_click(); check(u.IsWindowVisible(panel), 'orb can reopen repeatedly')
            # Folder selection is a real modal dialog; post the press so the driver remains responsive.
            folder=next(((x,770) for x in range(720,1200,20) if hit(x,770)==6),None)
            check(folder is not None, 'bottom-right library selector is clickable')
            u.PostMessageW(panel,0x201,1,packed(*folder))
            dialog=until(lambda: find_window(proc.pid,'#32770'),4)
            check(bool(dialog),'library switch opens the native folder chooser')
            u.PostMessageW(dialog,0x10,0,0)
            time.sleep(.4)
            send(0x202,*folder)
            # Reload uses the same visible preload flow and content-validated cache.
            before=len(log())
            click((rect.right-rect.left)/scale-150,20)
            until(lambda:'PRELOAD_READY' in log()[before:],90)
            print('RELOAD '+re.findall(r'PRELOAD_READY[^\r\n]+',log()[before:])[-1],flush=True)
            check(u.IsWindowVisible(panel),'reload completion keeps the working panel open')
            screenshot('03-reloaded.png')
            u.SendMessageW(panel,0x111,3,0)
            check(proc.wait(timeout=10)==0,'normal exit returns zero')
        finally:
            if proc.poll() is None:
                if panel:u.PostMessageW(panel,0x111,3,0)
                try:proc.wait(timeout=10)
                except subprocess.TimeoutExpired:proc.terminate();proc.wait(timeout=5)
            (out/'checks.json').write_text(json.dumps(checks,indent=2),encoding='utf-8')
    print(f'{len(checks)} checks passed',flush=True)
if __name__=='__main__':main()
