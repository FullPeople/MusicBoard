"""Real decoder tests against authored HTTP disconnects and delayed responses."""
from pathlib import Path
import hashlib, http.server, json, socket, subprocess, threading, time, uuid

root=Path(__file__).resolve().parents[1]
out=root/'artifacts'/('online-reconnect-'+uuid.uuid4().hex);out.mkdir()
fixtures=root/'artifacts/online-validation-truncated-preparation'
tools=root/'.cache/media-tools';probe=root/'build/windows-x64/Release/online_audio_tests.exe'
events=[];lock=threading.Lock();interrupted=set()
class Server(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        name=self.path.lstrip('/')
        if name=='stall.wav':
            self.send_response(200);self.send_header('Content-Type','audio/wav');self.send_header('Content-Length','999999');self.end_headers()
            time.sleep(2);return
        if name not in ('tone.wav','tone.mp3','short.wav'):
            self.send_error(404);return
        data=(fixtures/name).read_bytes()
        header=self.headers.get('Range','');start=0
        if header:
            if not header.startswith('bytes=') or not header.endswith('-'):
                self.send_error(416);return
            start=int(header[6:-1])
        if start>=len(data):self.send_error(416);return
        self.send_response(206 if start else 200)
        self.send_header('Content-Type','audio/wav' if name.endswith('.wav') else 'audio/mpeg')
        self.send_header('Accept-Ranges','bytes');self.send_header('Content-Length',str(len(data)-start))
        if start:self.send_header('Content-Range',f'bytes {start}-{len(data)-1}/{len(data)}')
        self.end_headers()
        with lock:
            drop=name.startswith('tone.') and name not in interrupted and start==0
            if drop:interrupted.add(name)
            events.append({'Name':name,'RangeStart':start,'InjectedDisconnect':drop})
        try:
            if drop:
                self.wfile.write(data[:96000 if name.endswith('.wav') else 2048]);self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
            for offset in range(start,len(data),65536):
                self.wfile.write(data[offset:offset+65536]);self.wfile.flush()
                # Bounded delayed delivery; a healthy connection receives bytes often.
                if name.startswith('tone.'):time.sleep(.05)
        except (BrokenPipeError,ConnectionResetError,OSError):pass

server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Server)
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
results={};failure=None
try:
    for name in ('tone.wav','tone.mp3'):
        with (out/(name+'.log')).open('wb') as log:
            base=f'http://127.0.0.1:{server.server_port}/'
            run=subprocess.run([str(probe),str(tools),base+name,base+'short.wav',base+'stall.wav'],stdout=log,stderr=subprocess.STDOUT,timeout=100)
        results[name]={'Passed':run.returncode==0,'ExitCode':run.returncode}
except Exception as error:failure=repr(error)
finally:
    server.shutdown();server.server_close();thread.join(timeout=5)
    with probe.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
    checks={'NativeDecoderAssertions':len(results)==2 and all(r['Passed'] for r in results.values()),
        'BothAuthoredConnectionsActuallyInterrupted':len(interrupted)==2,
        'BothSourcesActuallyResumedWithRange':all(any(e['Name']==name and e['RangeStart']>0 for e in events) for name in ('tone.wav','tone.mp3'))}
    report={'Passed':failure is None and all(checks.values()),'Checks':checks,'Results':results,'Failure':failure,'HTTPEvents':events,'ProbeSHA256':digest,
        'Scope':'Existing native decoder probe against authored WAV/MP3 HTTP connections, injected first disconnect and delayed chunks; not public-platform or long-outage acceptance'}
    (out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps({'Passed':report['Passed'],'Checks':checks,'Evidence':str(out)}),flush=True)
raise SystemExit(0 if report['Passed'] else 1)
