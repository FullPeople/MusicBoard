"""Owned HTTP audio, exact streaming boundary, cancellation and application input."""
from pathlib import Path
import functools
import http.server
import json
import math
import os
import struct
import subprocess
import threading
import wave

root=Path(__file__).resolve().parents[1]
out=root/'artifacts/online-validation'
out.mkdir(parents=True,exist_ok=False)
with wave.open(str(out/'tone.wav'),'wb') as audio:
    audio.setnchannels(2);audio.setsampwidth(2);audio.setframerate(48000)
    for second in range(30):
        audio.writeframes(b''.join(struct.pack('<hh',*(int(2500*math.sin(2*math.pi*733*(second*48000+i)/48000)),)*2) for i in range(48000)))
tools=root/'.cache/media-tools'
subprocess.run([str(tools/'ffmpeg.exe'),'-hide_banner','-loglevel','error','-i',str(out/'tone.wav'),'-c:a','libmp3lame',str(out/'tone.mp3')],check=True,timeout=30)
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self,*args):pass
    def copyfile(self,*args):
        try:super().copyfile(*args)
        except (ConnectionResetError,BrokenPipeError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Quiet,directory=str(out)))
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
checks={}
try:
    for name in ['tone.wav','tone.mp3']:
        with (out/(name+'.log')).open('wb') as log:
            result=subprocess.run([str(root/'build/windows-x64/Release/online_audio_tests.exe'),str(tools),f'http://127.0.0.1:{server.server_port}/{name}'],stdout=log,stderr=subprocess.STDOUT,timeout=50)
        checks[name]=result.returncode==0
    library=out/'library';(library/'BGM').mkdir(parents=True);(library/'SFX').mkdir()
    profile=out/'isolated-localappdata';profile.mkdir()
    with (out/'application.log').open('wb') as log:
        result=subprocess.run([str(root/'build/windows-x64/Release/musicboard.exe'),'--library',str(library),'--selftest-online',f'http://127.0.0.1:{server.server_port}/tone.mp3'],env=dict(os.environ,LOCALAPPDATA=str(profile)),stdout=log,stderr=subprocess.STDOUT,timeout=65)
    checks['NativeOnlineInputAndPlayback']=result.returncode==0
    checks['NoDownloadedLibraryFiles']=not any(p.is_file() for p in library.rglob('*'))
finally:
    server.shutdown();server.server_close();thread.join(timeout=5)
    (out/'report.json').write_text(json.dumps({'Passed':bool(checks) and all(checks.values()),'Checks':checks,'Scope':'Authored local HTTP WAV/MP3; real native controls and audio device when available. Not platform or cross-network acceptance.'},indent=2),encoding='utf-8')
print((out/'report.json').read_text());raise SystemExit(0 if checks and all(checks.values()) else 1)
