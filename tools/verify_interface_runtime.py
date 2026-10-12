"""Native window-message regression for the September 20 interface refinement.

Uses an isolated profile and read-only real library. Screenshots capture the
displayed desktop; cursor verification moves the pointer and restores it.
"""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import re
import subprocess
import time
from PIL import ImageGrab
from verify_runtime import find_window, until, u, w
from verify_dj_runtime import report_memory

u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.LoadCursorW.argtypes = [w.HINSTANCE, c.c_void_p]
u.LoadCursorW.restype = w.HANDLE
u.GetCursor.restype = w.HANDLE
u.GetWindowLongPtrW.argtypes = [w.HWND,c.c_int]
u.GetWindowLongPtrW.restype = c.c_ssize_t
u.GetCursorPos.argtypes = [c.POINTER(w.POINT)]
u.SetCursorPos.argtypes = [c.c_int, c.c_int]

class CursorInfo(c.Structure):
    _fields_ = [('size',w.DWORD),('flags',w.DWORD),('cursor',w.HANDLE),('position',w.POINT)]
u.GetCursorInfo.argtypes = [c.POINTER(CursorInfo)]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--exe',required=True)
    parser.add_argument('--library',required=True)
    parser.add_argument('--out',required=True)
    parser.add_argument('--profile')
    parser.add_argument('--startup-only',action='store_true')
    parser.add_argument('--expect-language', choices=['cn','en'], default='cn')
    args=parser.parse_args()
    out=Path(args.out).resolve(); out.mkdir(parents=True,exist_ok=True)
    profile=Path(args.profile).resolve() if args.profile else out/'profile'
    profile.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,LOCALAPPDATA=str(profile),MUSICBOARD_TRACE_INPUT='1')
    checks=[]; panel=None
    pointer=w.POINT(); u.GetCursorPos(c.byref(pointer))
    def check(value,label):
        checks.append(dict(pass_=bool(value),label=label))
        print(('PASS ' if value else 'FAIL ')+label,flush=True)
        if not value: raise AssertionError(label)
    def logs():
        return ''.join(p.read_text(encoding='utf-8',errors='replace') for p in sorted((profile/'MusicBoard'/'logs').glob('app-*.log')))
    previous=len(logs())
    def log(): return logs()[previous:]
    with (out/'stdout.txt').open('wb') as stdout,(out/'stderr.txt').open('wb') as stderr:
        started=time.monotonic()
        proc=subprocess.Popen([str(Path(args.exe).resolve()),'--library',args.library],env=env,stdout=stdout,stderr=stderr)
        try:
            panel=until(lambda:find_window(proc.pid,'MusicBoard.CompositionWindow'))
            orb=until(lambda:find_window(proc.pid,'MusicBoard.OrbWindow'))
            until(lambda:'PRELOAD_READY' in log() or proc.poll() is not None,60)
            check(proc.poll() is None and u.IsWindowVisible(panel) and u.IsWindowVisible(orb),'startup opens panel and keeps orb visible')
            print(re.findall(r'PRELOAD_READY[^\r\n]+',log())[-1]+f' wall_seconds={time.monotonic()-started:.3f}',flush=True)
            report_memory(proc)
            if args.startup_only:
                config=(profile/'MusicBoard'/'settings.conf').read_text(encoding='utf-8')
                check('show_all_categories=0' in config, 'startup preserves default-off category switch')
                check('schema=7' in config, 'startup saves current settings schema')
                check('language='+args.expect_language in config, 'startup preserves selected language')
                u.PostMessageW(panel,0x111,3,0)
                check(proc.wait(timeout=10)==0,'startup measurement exits normally')
                return
            u.SetWindowPos(panel,None,40,40,0,0,0x15)
            u.SetWindowPos(orb,c.c_void_p(-1),1350,100,0,0,0x15)
            u.SetForegroundWindow(panel); time.sleep(.4)
            scale=u.GetDpiForWindow(panel)/96
            def bounds(hwnd=panel):
                r=w.RECT(); u.GetWindowRect(hwnd,c.byref(r)); return r
            def pack(x,y,hwnd=panel):
                dpi=u.GetDpiForWindow(hwnd)/96
                return (round(y*dpi)&65535)<<16 | (round(x*dpi)&65535)
            def send(msg,x,y,wp=0,hwnd=panel):return u.SendMessageW(hwnd,msg,wp,pack(x,y,hwnd))
            def hit(x,y):
                send(0x200,x,y)
                ids=re.findall(r' id=(-?\d+)',log())
                return int(ids[-1]) if ids else -1
            def click(x,y,hwnd=panel):
                send(0x200,x,y,hwnd=hwnd);send(0x201,x,y,1,hwnd);send(0x202,x,y,hwnd=hwnd);time.sleep(.15)
            def screenshot(name,hwnd=panel):
                r=bounds(hwnd); pic=ImageGrab.grab(bbox=(r.left,r.top,r.right,r.bottom),all_screens=True)
                pic.save(out/name);return pic
            def locate(identity,xs,ys):
                return next(((x,y) for y in ys for x in xs if hit(x,y)==identity),None)
            def cursor(x,y,kind,hwnd=panel):
                r=bounds(hwnd);dpi=u.GetDpiForWindow(hwnd)/96
                u.SetCursorPos(r.left+round(x*dpi),r.top+round(y*dpi))
                send(0x200,x,y,hwnd=hwnd)
                u.SendMessageW(hwnd,0x20,hwnd,1|(0x200<<16));time.sleep(.08)
                info=CursorInfo();info.size=c.sizeof(info)
                if not u.GetCursorInfo(c.byref(info)):raise c.WinError(c.get_last_error())
                return info.cursor==u.LoadCursorW(None,c.c_void_p(kind))
            screenshot('01-open.png')
            click(1123,21)
            check('language=en' in (profile/'MusicBoard'/'settings.conf').read_text(encoding='utf-8'), 'EN click persists English')
            screenshot('00-english.png')
            click(1089,21)
            check('language=cn' in (profile/'MusicBoard'/'settings.conf').read_text(encoding='utf-8'), 'CN click restores Chinese')
            config_path = profile/'MusicBoard'/'settings.conf'
            check('show_all_categories=0' in config_path.read_text(encoding='utf-8'), 'show all defaults to off')
            check(hit(104,771)==10, 'show all switch has its own footer target')
            click(104,771)
            check('show_all_categories=1' in config_path.read_text(encoding='utf-8'), 'show all switch persists on')
            # Select All in each column for the existing playback regression.
            for identity in (400,500):
                xy=locate(identity,range(18,1230,10),range(335,565,4))
                check(xy is not None, 'all category available in both columns')
                click(*xy)
            check(hit(700,771)==-1,'library address has no click target')
            check(hit(1108,771)==6 and hit(1198,771)==5,'separate footer change and reload buttons')
            check(hit(1162,22)==3 and hit(1196,22)==4 and hit(1230,22)==2,'pin minimize close in requested order')
            r=bounds()
            check(all(u.SendMessageW(panel,0x84,0,((r.top+round(22*scale))&65535)<<16|((r.left+round(x*scale))&65535))==1 for x in (1162,1196,1230)),
                'native caption hit testing leaves all three title buttons clickable')
            pinned_before=bool(u.GetWindowLongPtrW(panel,-20)&8)
            click(1162,22);send(0x200,-100,-100);time.sleep(.12)
            pin_changed=screenshot('02-pin-toggled.png')
            check(bool(u.GetWindowLongPtrW(panel,-20)&8)!=pinned_before,'pin changes actual native topmost state')
            config=(profile/'MusicBoard'/'settings.conf').read_text(encoding='utf-8')
            check(f'panel_topmost={int(not pinned_before)}' in config,'pin persists its changed state')
            click(1162,22);send(0x200,-100,-100);time.sleep(.12)
            pin_restored=screenshot('02-pin-restored.png')
            check(bool(u.GetWindowLongPtrW(panel,-20)&8)==pinned_before,'second pin click restores native state')
            def amber_pixels(pic):
                return sum(1 for rgb in pic.crop((round(1148*scale),round(6*scale),round(1176*scale),round(36*scale))).getdata()
                           if rgb[0]>150 and rgb[0]>rgb[1]*1.15 and rgb[1]>rgb[2]*1.3)
            active,inactive=(pin_restored,pin_changed) if pinned_before else (pin_changed,pin_restored)
            check(amber_pixels(active)>20 and amber_pixels(inactive)<5,'actual pin pixels switch between amber and inactive gray')
            mode=locate(7,range(850,950,8),range(85,124,4));check(mode is not None,'mode selector available')
            click(800,103)
            first_effect=locate(2000,range(780,970,15),range(500,725,5))
            second_effect=locate(2001,range(1020,1200,15),range(500,725,5))
            check(first_effect is not None and second_effect is not None,'turntable library cards found for right-click assignment')
            for slot in range(1,5):
                before=len(log());send(0x205,*first_effect)
                until(lambda:f'status: 音效 {slot}  ' in log()[before:],3)
            check('status: 正在播放' not in log(),'four right-click assignments use SFX and leave BGM idle')
            before=len(log());send(0x205,*second_effect)
            until(lambda:'status: 音效 1  ' in log()[before:],3)
            check(True,'fifth right-click replaces SFX deck one')
            screenshot('02-sfx-assigned.png');click(877,66)
            click(890,103)
            check('DJ 按钮 ·' in log(),'DJ mode activates')
            points={}
            for y in range(175,720,5):
                for x in range(770,1220,10):
                    identity=hit(x,y)
                    if 2000<=identity<3000 and identity not in points:points[identity]=(x+10,y+15)
                if len(points)>=3:break
            first=sorted(points.items())[:3]
            check(len(first)==3 and len({p[1][1] for p in first})==1,'three independent DJ pads share one row')
            for _,xy in first:click(*xy)
            check(len(re.findall('PAD_START',log()))>=3,'three pads trigger separate voices')
            screenshot('03-dj.png')
            bgm=next(((100,y+8) for y in range(410,730,5) if 1000<=hit(100,y)<2000),None)
            check(bgm is not None,'BGM card found in live geometry')
            before=len(log());send(0x201,*bgm,1)
            for step in range(1,9):
                send(0x200,bgm[0]+(115-bgm[0])*step/8,bgm[1]+(190-bgm[1])*step/8,1);time.sleep(.03)
            send(0x202,115,190);until(lambda:'status: 正在播放' in log()[before:],4)
            check(cursor(115,190,32649),'disc circle uses hand cursor')
            click(1123,21);screenshot('04-english-playing.png');click(1089,21)
            time.sleep(.2);screenshot('04-disc-hover.png')
            check(cursor(32,108,32512),'outside circle keeps arrow cursor')
            search=locate(300,range(70,620,30),range(290,340,4));check(search is not None,'music search field found')
            check(cursor(*search,32513),'music search uses I-beam')
            click(*search)
            for char in '水边':u.SendMessageW(panel,0x102,ord(char),0)
            time.sleep(.16);search_picture=screenshot('05-search.png')
            vertical=max(sum(1 for y in range(round(304*scale),round(326*scale))
                if (lambda rgb:rgb[0]>160 and rgb[1]>90 and rgb[2]<100)(search_picture.getpixel((x,y))[:3]))
                for x in range(round(74*scale),round(84*scale)))
            check(vertical>=round(15*scale),'typed search displays a visible vertical caret in the captured window')
            for _ in '水边':u.SendMessageW(panel,0x102,8,0)
            sfxsearch=locate(301,range(850,1170,30),range(120,190,4));check(sfxsearch is not None,'SFX search field found')
            check(cursor(*sfxsearch,32513),'SFX search uses I-beam')
            click(*sfxsearch);time.sleep(.16);screenshot('06-sfx-search.png')
            check(cursor(92,85,32649,orb),'orb upper region uses hand cursor')
            time.sleep(.2);screenshot('07-orb-top.png',orb)
            check(cursor(92,127,32649,orb),'orb lower region uses hand cursor')
            before=len(log());click(92,127,orb)
            until(lambda:'status: 已暂停' in log()[before:],3)
            check(u.IsWindowVisible(panel),'orb lower pause leaves panel open')
            time.sleep(.35);screenshot('08-orb-paused.png',orb);screenshot('09-paused.png')
            before=len(log());click(92,127,orb);until(lambda:'status: 继续  ' in log()[before:],3)
            check(True,'orb lower region resumes BGM')
            click(1230,22);until(lambda:not u.IsWindowVisible(panel),3)
            check(proc.poll() is None,'close hides panel and keeps process alive')
            click(92,85,orb);check(u.IsWindowVisible(panel),'orb upper region opens hidden panel')
            click(92,85,orb)
            check(not u.IsWindowVisible(panel) and u.IsWindowVisible(orb),'orb upper region hides panel before returning from click')
            click(92,85,orb);check(u.IsWindowVisible(panel),'orb reopens after toggling closed')
            click(26,771);r=bounds();check(abs((r.right-r.left)/scale-746)<1,'collapse physically narrows window to 746 DIP')
            check(hit(652,22)==3 and hit(686,22)==4 and hit(720,22)==2,'title controls remain correct after collapse')
            screenshot('10-folded.png')
            click(26,771);r=bounds();check(abs((r.right-r.left)/scale-1256)<1,'expand restores 1256 DIP')
            before=len(re.findall('PRELOAD_READY',log()));reload_start=time.monotonic()
            click(1198,771)
            until(lambda:len(re.findall('PRELOAD_READY',log()))>before,30)
            print(re.findall(r'PRELOAD_READY[^\r\n]+',log())[-1]+f' reload_wall_seconds={time.monotonic()-reload_start:.3f}',flush=True)
            check(u.IsWindowVisible(panel),'footer reload completes with panel open')
            u.PostMessageW(panel,0x201,1,pack(1108,771))
            dialog=until(lambda:find_window(proc.pid,'#32770'),4)
            check(bool(dialog),'change library button opens the native folder chooser')
            u.PostMessageW(dialog,0x10,0,0);time.sleep(.4);send(0x202,1108,771)
            click(104,771)
            check('show_all_categories=0' in config_path.read_text(encoding='utf-8'), 'show all switches back off and persists')
            screenshot('12-all-off.png')
            click(1123,21)
            check('language=en' in config_path.read_text(encoding='utf-8'), 'English remains saved for restart check')
            # Context menu runs its own message loop: post instead of blocking SendMessage.
            r=bounds(orb);u.SetCursorPos(r.left+92,r.top+85)
            u.PostMessageW(orb,0x205,0,pack(92,85,orb));time.sleep(.3)
            ImageGrab.grab(bbox=(r.left+70,r.top+60,r.left+580,r.top+380),all_screens=True).save(out/'11-context-menu.png')
            u.PostMessageW(panel,0x100,27,0);u.PostMessageW(orb,0x100,27,0)
            u.mouse_event(0x0002,0,0,0,0);u.mouse_event(0x0004,0,0,0,0);time.sleep(.15)
            u.PostMessageW(panel,0x111,3,0)
            check(proc.wait(timeout=10)==0,'normal process exit is zero')
        finally:
            if proc.poll() is None:
                if panel:u.PostMessageW(panel,0x111,3,0)
                try:proc.wait(timeout=5)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
            u.SetCursorPos(pointer.x,pointer.y)
            (out/'checks.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2),encoding='utf-8')

if __name__=='__main__':main()
