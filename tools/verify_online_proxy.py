"""Force real native decoding through an owned HTTP proxy, using authored audio."""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import urllib.parse
import uuid


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("probe", "tools", "fixtures", "output-root"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    tools = args.tools.resolve(strict=True)
    fixtures = args.fixtures.resolve(strict=True)
    out = args.output_root.resolve() / uuid.uuid4().hex
    out.mkdir(parents=True, exist_ok=False)
    events = []
    lock = threading.Lock()
    host = "musicboard-proxy-fixture.invalid"

    class Proxy(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            target = urllib.parse.urlsplit(self.path)
            name = target.path.lstrip("/")
            with lock:
                events.append({"Host": target.hostname, "Name": name, "Range": self.headers.get("Range", "")})
            if target.hostname != host:
                self.send_error(400)
                return
            if name == "stall.wav":
                self.send_response(200)
                self.send_header("Content-Type", "audio/wav")
                self.send_header("Content-Length", "999999")
                self.end_headers()
                time.sleep(2)
                return
            if name not in ("tone.wav", "tone.mp3", "short.wav"):
                self.send_error(404)
                return
            data = (fixtures / name).read_bytes()
            header = self.headers.get("Range", "")
            start = 0
            if header:
                if not header.startswith("bytes=") or not header.endswith("-"):
                    self.send_error(416)
                    return
                start = int(header[6:-1])
            if start >= len(data):
                self.send_error(416)
                return
            self.send_response(206 if start else 200)
            self.send_header("Content-Type", "audio/wav" if name.endswith("wav") else "audio/mpeg")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(len(data) - start))
            if start:
                self.send_header("Content-Range", f"bytes {start}-{len(data)-1}/{len(data)}")
            self.end_headers()
            try:
                for offset in range(start, len(data), 65536):
                    self.wfile.write(data[offset:offset + 65536])
                    self.wfile.flush()
            except (OSError, BrokenPipeError, ConnectionResetError):
                pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = dict(os.environ, http_proxy=f"http://127.0.0.1:{server.server_port}", no_proxy="", NO_PROXY="")
    results = []
    failure = None
    try:
        for name in ("tone.wav", "tone.mp3"):
            with (out / (name + ".log")).open("wb") as log:
                process = subprocess.Popen([str(probe), str(tools), f"http://{host}/{name}",
                    f"http://{host}/short.wav", f"http://{host}/stall.wav"], env=env,
                    stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    code = process.wait(timeout=100)
                except subprocess.TimeoutExpired:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=10)
                    raise
            lines = (out / (name + ".log")).read_text(encoding="utf-8", errors="replace")
            results.append({"Source": name, "Passed": code == 0 and "SKIP " not in lines,
                "ExitCode": code, "OwnedPid": process.pid})
    except Exception as error:
        failure = repr(error)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    checks = {"NativeWAVAndMP3Assertions": len(results) == 2 and all(r["Passed"] for r in results),
        "BothSourcesActuallyRequestedThroughProxy": all(any(e["Name"] == name for e in events) for name in ("tone.wav", "tone.mp3")),
        "AllRequestsStayOnAuthoredProxyTarget": bool(events) and all(e["Host"] == host for e in events),
        "OwnedProxyStopped": not thread.is_alive()}
    report = {"Passed": failure is None and all(checks.values()), "Checks": checks, "Results": results,
        "Failure": failure, "HTTPEvents": events, "ProbeSHA256": hashlib.sha256(probe.read_bytes()).hexdigest(),
        "Scope": "Real FFmpeg and XAudio2 against authored WAV/MP3 over an owned HTTP proxy; synthetic .invalid target requires proxy transport, not public-platform or long-outage acceptance"}
    (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"Passed": report["Passed"], "Checks": checks, "Evidence": str(out / "report.json")}), flush=True)
    return 0 if report["Passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
