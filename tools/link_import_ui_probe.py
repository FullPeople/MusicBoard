"""Exercise the real modeless link dialog with an isolated library and HTTP fixture."""
import hashlib
import argparse
import math
import array
import sys
import tempfile
import shutil
import io
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import uuid
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import win32con
import win32gui
import win32process

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--exe", type=Path, default=root / "build/windows-x64/Release/musicboard.exe")
parser.add_argument("--autoplay-tone", action="store_true", help="Measure only a generated tone; never retain loopback audio")
parser.add_argument("--python-modules", type=Path, default=root / "build/qa-python")
parser.add_argument("--profile-base", type=Path, help="Put the isolated app profile on a chosen volume")
parser.add_argument("--fixture-megabytes", type=int, default=0, help="Generate 8-128 MiB of silent audio for cross-volume UI checks")
parser.add_argument("--baseline", action="store_true", help="Record an older app's behavior without asserting the new publication response")
args = parser.parse_args()
if args.fixture_megabytes and not 8 <= args.fixture_megabytes <= 128:
    parser.error("fixture size must be 8-128 MiB")
if args.fixture_megabytes and args.autoplay_tone:
    parser.error("large silent fixture and tone output check are separate cases")
out = root / "build/windows-x64/link-ui-probe" / uuid.uuid4().hex
out.mkdir(parents=True)
library = out / "library"
for part in ("BGM", "SFX"):
    (library / part).mkdir(parents=True)
buffer = io.BytesIO()
with wave.open(buffer, "wb") as fixture:
    fixture.setnchannels(2)
    fixture.setsampwidth(2)
    fixture.setframerate(44100)
    if args.autoplay_tone:
        samples = array.array("h", (int(8000 * math.sin(2 * math.pi * 997 * i / 44100))
                                  for i in range(44100 * 2) for _ in range(2)))
        fixture.writeframes(samples.tobytes())
    elif args.fixture_megabytes:
        fixture.writeframes(bytes(args.fixture_megabytes * 1024 * 1024))
    else:
        fixture.writeframes(bytes(44100 * 4 * 2))
audio = buffer.getvalue()
measurements = []
capture = capture_stream = None
if args.autoplay_tone:
    import numpy as np
    sys.path.insert(0, str(args.python_modules.resolve()))
    import pyaudiowpatch as pa
    capture = pa.PyAudio()
    info = capture.get_default_wasapi_loopback()
    rate, channels = int(info["defaultSampleRate"]), int(info["maxInputChannels"])
    frames = round(rate * 0.05)
    kernel = np.exp(-2j * np.pi * 997 * np.arange(frames) / rate)
    def measured(data, count, times, flags):
        if count == frames:
            pcm = np.frombuffer(data, dtype=np.float32).reshape(-1, channels).mean(axis=1)
            measurements.append((time.monotonic(), float(2 * abs(kernel @ pcm) / frames)))
        return None, pa.paContinue
    capture_stream = capture.open(format=pa.paFloat32, channels=channels, rate=rate, input=True,
        input_device_index=info["index"], frames_per_buffer=frames, stream_callback=measured)
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(audio)))
        self.end_headers()
        try:
            size = 65536 if args.fixture_megabytes else 4096
            for offset in range(0, len(audio), size):
                self.wfile.write(audio[offset:offset + size])
                self.wfile.flush()
                time.sleep(0.001 if args.fixture_megabytes else 0.02)
        except (ConnectionError, OSError):
            pass
    def log_message(self, *_):
        pass
server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
profile = (Path(tempfile.mkdtemp(prefix="musicboard-url-profile-", dir=args.profile_base.resolve(strict=True)))
           if args.profile_base else out / "profile")
env = dict(os.environ, LOCALAPPDATA=str(profile))
process = subprocess.Popen([str(args.exe.resolve(strict=True)), "--library", str(library)],
    env=env, creationflags=subprocess.CREATE_NO_WINDOW)
checks = {}
panel = 0
def owned(kind):
    found = []
    win32gui.EnumWindows(lambda window, _: found.append(window)
        if win32process.GetWindowThreadProcessId(window)[1] == process.pid and win32gui.GetClassName(window) == kind
            and win32gui.IsWindowVisible(window) else None, None)
    return found[0] if found else 0
def wait(condition, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("Owned app exited early: " + str(process.returncode))
        value = condition()
        if value:
            return value
        time.sleep(0.03)
    raise TimeoutError("Native link dialog did not reach the expected state")
try:
    panel = wait(lambda: owned("MusicBoard.CompositionWindow"))
    time.sleep(0.3)
    win32gui.PostMessage(panel, win32con.WM_COMMAND, 20, 0)
    dialog = wait(lambda: owned("MusicBoard.LinkImport"))
    controls = {i: win32gui.GetDlgItem(dialog, i) for i in range(101, 108)}
    checks["RealTrayActionOpensLinkDialog"] = all(controls.values())
    win32gui.SendMessage(controls[101], win32con.WM_SETTEXT, 0, "not a URL")
    win32gui.SendMessage(controls[106], win32con.BM_CLICK, 0, 0)
    wait(lambda: "完整" in win32gui.GetWindowText(controls[105]) or "格式" in win32gui.GetWindowText(controls[105]))
    checks["InvalidUrlRemainsEditable"] = bool(win32gui.IsWindowEnabled(controls[106]))
    link = "http://127.0.0.1:" + str(server.server_port) + "/silence.wav"
    win32gui.SendMessage(controls[101], win32con.WM_SETTEXT, 0, link)
    win32gui.SendMessage(controls[102], win32con.WM_SETTEXT, 0, "链接验证")
    win32gui.SendMessage(controls[104], win32con.BM_SETCHECK,
                         win32con.BST_CHECKED if args.autoplay_tone else win32con.BST_UNCHECKED, 0)
    win32gui.SendMessage(controls[106], win32con.BM_CLICK, 0, 0)
    wait(lambda: not win32gui.IsWindowEnabled(controls[106]))
    checks["DownloadDisablesDuplicateSubmission"] = not win32gui.IsWindowEnabled(controls[101])
    progress = set()
    responsiveness = []
    began = time.monotonic()
    while not win32gui.IsWindowEnabled(controls[106]) and time.monotonic() - began < 25:
        progress.add(win32gui.GetWindowText(controls[105]))
        input_start = time.perf_counter()
        win32gui.SendMessageTimeout(panel, win32con.WM_NULL, 0, 0, win32con.SMTO_ABORTIFHUNG, 1000)
        responsiveness.append((time.perf_counter() - input_start) * 1000)
        time.sleep(0.01 if args.fixture_megabytes else 0.04)
    saved = list((library / "BGM/链接验证").glob("*.wav"))
    checks["DownloadProgressChanges"] = len(progress) > 2
    checks["OfflineFileRetained"] = len(saved) == 1
    checks["OfflineFileExactlyMatchesFixture"] = len(saved) == 1 and hashlib.sha256(saved[0].read_bytes()).digest() == hashlib.sha256(audio).digest()
    checks["CompletedDownloadReenablesControls"] = bool(win32gui.IsWindowEnabled(controls[106]))
    if args.fixture_megabytes and not args.baseline:
        checks["InputRespondsDuringDownloadAndPublication"] = bool(responsiveness) and max(responsiveness) < 100
        checks["PublicationStageObserved"] = any("曲库" in state or "准备播放" in state for state in progress)
    if args.autoplay_tone:
        baseline = max((level for stamp, level in measurements if stamp < began), default=0)
        threshold = max(0.01, baseline * 10)
        wait(lambda: any(amplitude > threshold for stamp, amplitude in measurements if stamp >= began), seconds=5)
        checks["GeneratedImportedToneReachesActualAudioOutput"] = any(
            amplitude > threshold for stamp, amplitude in measurements if stamp >= began)
        time.sleep(0.1)
    win32gui.SendMessage(controls[102], win32con.WM_SETTEXT, 0, "取消验证")
    win32gui.SendMessage(controls[106], win32con.BM_CLICK, 0, 0)
    wait(lambda: not win32gui.IsWindowEnabled(controls[106]))
    cancel_start = time.monotonic()
    win32gui.SendMessage(controls[107], win32con.BM_CLICK, 0, 0)
    wait(lambda: win32gui.IsWindowEnabled(controls[106]), seconds=5)
    checks["CancelledDownloadCreatesNoLibraryFile"] = not list((library / "BGM/取消验证").glob("*"))
    checks["CancelReturnsToEditableState"] = bool(win32gui.IsWindowEnabled(controls[101]))
    report = {"Passed": all(checks.values()), "Checks": checks,
        "ProgressStates": len(progress), "CancelMilliseconds": round((time.monotonic() - cancel_start) * 1000, 1),
        "TestScope": "Real Windows app; loopback HTTP; isolated library; silent fixture; no public platform claim"}
    report["EvidenceDirectory"] = str(out)
    report["ProfileDirectory"] = str(profile)
    report["FixtureBytes"] = len(audio)
    report["InputRoundTrips"] = len(responsiveness)
    report["InputMaximumMilliseconds"] = max(responsiveness, default=0)
    report["ProgressText"] = sorted(progress)
    report["Baseline"] = args.baseline
    if args.autoplay_tone:
        report["TestScope"] = "Real Windows app; generated tone via URL import and autoplay; actual WASAPI loopback; spectral amplitudes only"
        report["ToneAmplitudePeak"] = max((level for _, level in measurements), default=0)
        report["ToneAmplitudeBaseline"] = baseline
    (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report), flush=True)
    if not report["Passed"]:
        raise RuntimeError("Native URL UI check failed: " + str(out))
except Exception as error:
    diagnostic = {"Error": str(error), "Checks": checks}
    if "controls" in globals() and controls.get(105):
        diagnostic["DialogStatus"] = win32gui.GetWindowText(controls[105])
    (out / "failure.json").write_text(json.dumps(diagnostic, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(diagnostic, ensure_ascii=False), flush=True)
    raise
finally:
    if panel and process.poll() is None:
        win32gui.PostMessage(panel, win32con.WM_COMMAND, 3, 0)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=5)
    server.shutdown()
    if capture_stream is not None:
        capture_stream.stop_stream()
        capture_stream.close()
    if capture is not None:
        capture.terminate()
    if args.profile_base:
        resolved = profile.resolve()
        if resolved.parent != args.profile_base.resolve(strict=True) or not resolved.name.startswith("musicboard-url-profile-"):
            raise RuntimeError("Profile cleanup escaped its owned root")
        if any(path.is_symlink() or path.is_junction() for path in resolved.rglob("*")):
            raise RuntimeError("Profile cleanup encountered a linked path")
        shutil.rmtree(resolved)
