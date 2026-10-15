"""Exercise actual app playback against authored files and a range-capable HTTP server."""
from pathlib import Path
import contextlib, functools, http.server, json, math, os, plistlib, socketserver, struct, subprocess, threading, wave

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'artifacts/macos'
FIXTURE = OUT/'http-fixture'; FIXTURE.mkdir(exist_ok=False)
audio = FIXTURE/'authored.wav'
with wave.open(str(audio),'wb') as stream:
    stream.setnchannels(2); stream.setsampwidth(2); stream.setframerate(44100)
    stream.writeframes(b''.join(struct.pack('<hh',int(math.sin(i*2*math.pi*440/44100)*5000),int(math.sin(i*2*math.pi*440/44100)*5000)) for i in range(44100*8)))

class Handler(http.server.BaseHTTPRequestHandler):
    def do_HEAD(self): self.serve(False)
    def do_GET(self): self.serve(True)
    def serve(self, body):
        if self.path.split('?')[0] != '/authored.wav': self.send_error(404); return
        data = audio.read_bytes(); start, end = 0, len(data)-1
        if self.headers.get('Range'):
            value=self.headers['Range'].split('=')[1].split('-'); start=int(value[0] or 0); end=min(int(value[1]) if value[1] else end,end)
            self.send_response(206); self.send_header('Content-Range',f'bytes {start}-{end}/{len(data)}')
        else: self.send_response(200)
        self.send_header('Content-Type','audio/wav'); self.send_header('Accept-Ranges','bytes'); self.send_header('Content-Length',str(end-start+1)); self.end_headers()
        if body:
            try: self.wfile.write(data[start:end+1])
            except (BrokenPipeError,ConnectionResetError): pass
    def log_message(self, *_): pass

with socketserver.ThreadingTCPServer(('127.0.0.1',0),Handler) as server:
    thread=threading.Thread(target=server.serve_forever,daemon=True); thread.start()
    try:
        subprocess.run([str(OUT/'MusicBoard.app/Contents/MacOS/MusicBoard'),'--profile',str(OUT/'owned-test-profile'),'--self-test',str(OUT/'native-checks'),'--fixture-url',f'http://127.0.0.1:{server.server_address[1]}/authored.wav'],check=True,timeout=200)
    finally: server.shutdown(); thread.join(timeout=5)
report=json.loads((OUT/'native-checks/report.json').read_text())
if not report['Passed'] or not all(report['Checks'].values()): raise RuntimeError('Actual native checks failed')
info=plistlib.loads((OUT/'MusicBoard.app/Contents/Info.plist').read_bytes())
for key in ['NSMicrophoneUsageDescription','NSCameraUsageDescription','NSAppleEventsUsageDescription','NSScreenCaptureUsageDescription']:
    if key in info: raise RuntimeError('Unexpected privacy request: '+key)
subprocess.run(['codesign','--verify','--deep','--strict',str(OUT/'MusicBoard.app')],check=True)
print(json.dumps({'Passed':True,'NativeChecks':len(report['Checks']),'NoMicrophoneCameraScreenOrAutomationUsageKeys':True}))
