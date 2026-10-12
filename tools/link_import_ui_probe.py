"""Exercise the real modeless link dialog with an isolated library and HTTP fixture."""
import hashlib
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
    fixture.writeframes(bytes(44100 * 4 * 2))
audio = buffer.getvalue()
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(audio)))
        self.end_headers()
        try:
            for offset in range(0, len(audio), 4096):
                self.wfile.write(audio[offset:offset + 4096])
                self.wfile.flush()
                time.sleep(0.02)
        except (ConnectionError, OSError):
            pass
    def log_message(self, *_):
        pass
server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
env = dict(os.environ, LOCALAPPDATA=str(out / "profile"))
process = subprocess.Popen([str(root / "build/windows-x64/Release/musicboard.exe"), "--library", str(library)],
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
    wait(lambda: "import.url" in win32gui.GetWindowText(controls[105]))
    checks["InvalidUrlRemainsEditable"] = bool(win32gui.IsWindowEnabled(controls[106]))
    link = "http://127.0.0.1:" + str(server.server_port) + "/silence.wav"
    win32gui.SendMessage(controls[101], win32con.WM_SETTEXT, 0, link)
    win32gui.SendMessage(controls[102], win32con.WM_SETTEXT, 0, "链接验证")
    win32gui.SendMessage(controls[104], win32con.BM_SETCHECK, win32con.BST_UNCHECKED, 0)
    win32gui.SendMessage(controls[106], win32con.BM_CLICK, 0, 0)
    wait(lambda: not win32gui.IsWindowEnabled(controls[106]))
    checks["DownloadDisablesDuplicateSubmission"] = not win32gui.IsWindowEnabled(controls[101])
    progress = set()
    began = time.monotonic()
    while not win32gui.IsWindowEnabled(controls[106]) and time.monotonic() - began < 25:
        progress.add(win32gui.GetWindowText(controls[105]))
        time.sleep(0.04)
    saved = list((library / "BGM/链接验证").glob("*.wav"))
    checks["DownloadProgressChanges"] = len(progress) > 2
    checks["OfflineFileRetained"] = len(saved) == 1
    checks["OfflineFileExactlyMatchesFixture"] = len(saved) == 1 and hashlib.sha256(saved[0].read_bytes()).digest() == hashlib.sha256(audio).digest()
    checks["CompletedDownloadReenablesControls"] = bool(win32gui.IsWindowEnabled(controls[106]))
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
