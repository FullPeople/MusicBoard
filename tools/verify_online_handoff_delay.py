"""Expose a second decoder connection delay after the native preload completes."""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import uuid

from verify_public_online_audio import EXPECTED


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    data = args.fixture.resolve(strict=True).read_bytes()
    out = args.output_root.resolve() / uuid.uuid4().hex
    (out / "library/BGM").mkdir(parents=True, exist_ok=False)
    (out / "library/SFX").mkdir()
    events = []
    lock = threading.Lock()

    class Server(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.path != "/tone.wav":
                self.send_error(404)
                return
            with lock:
                number = len(events) + 1
                delayed = number >= 3
                events.append({"Request": number, "DelaySeconds": 3 if delayed else 0,
                    "Range": self.headers.get("Range", ""), "Time": time.monotonic()})
            if delayed:
                time.sleep(3)
            start = 0
            header = self.headers.get("Range", "")
            if header:
                if not header.startswith("bytes=") or not header.endswith("-"):
                    self.send_error(416)
                    return
                start = int(header[6:-1])
            if start >= len(data):
                self.send_error(416)
                return
            self.send_response(206 if start else 200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(len(data) - start))
            if start:
                self.send_header("Content-Range", f"bytes {start}-{len(data)-1}/{len(data)}")
            self.end_headers()
            try:
                for offset in range(start, len(data), 65536):
                    self.wfile.write(data[offset:offset + 65536])
                    self.wfile.flush()
            except OSError:
                pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Server)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with (out / "native.log").open("wb") as log:
            process = subprocess.Popen([str(exe), "--library", str(out / "library"), "--selftest-online",
                f"http://127.0.0.1:{server.server_port}/tone.wav"],
                env=dict(os.environ, LOCALAPPDATA=str(out / "profile"), no_proxy="127.0.0.1", NO_PROXY="127.0.0.1"),
                stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                code = process.wait(timeout=70)
            except subprocess.TimeoutExpired:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)
                raise
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    lines = (out / "native.log").read_text(encoding="utf-8", errors="replace").splitlines()
    checks = {label: "PASS " + label in lines for label in EXPECTED}
    checks["NativeAudioOutputNotSkipped"] = not any(line.startswith("SKIP ") for line in lines)
    checks["NoDownloadedLibraryFiles"] = not any(p.is_file() for p in (out / "library").rglob("*"))
    report = {"Passed": code == 0 and all(checks.values()), "Checks": checks, "ExitCode": code,
        "OwnedPid": process.pid, "HTTPEvents": events,
        "PlaybackPosition": [line for line in lines if line.startswith("ONLINE_PLAYBACK_POSITION")],
        "Scope": "Native online import and XAudio2 after authored delay on the third and subsequent media requests; original 4-second observation and >3-second playback requirement unchanged"}
    (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"Passed": report["Passed"], "HTTPEvents": events,
        "PlaybackPosition": report["PlaybackPosition"], "Evidence": str(out / "report.json")}), flush=True)
    return 0 if report["Passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
