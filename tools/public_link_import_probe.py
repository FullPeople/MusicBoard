"""Exercise public providers through the shipped dialog in an isolated library.

Downloads stay in the private evidence folder. Success requires a valid audio
file, not just recognizing the provider's hostname. Playback needs a separate
audio-output check. No browser cookies or user library are accessed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

CASES = {
    "bilibili": "https://www.bilibili.com/video/BV13x41117TL",
    "netease": "https://music.163.com/#/song?id=550136151",
    "qqmusic": "https://y.qq.com/n/ryqq/songDetail/004Ti8rT003TaZ",
    "youtube": "https://www.youtube.com/watch?v=YE7VzlLtp-4",
    "vimeo": "https://vimeo.com/76979871",
    "soundcloud": "https://soundcloud.com/ethmusic/lostin-powers-she-so-heavy",
    "bandcamp": "https://youtube-dl.bandcamp.com/track/youtube-dl-test-song",
    "kuwo": "https://www.kuwo.cn/yinyue/635632/",
    "kugou": "https://www.kugou.com/song-36/2pbbjscd.html",
}


def main():
    import win32con
    import win32gui
    import win32process

    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--timeout", type=float, default=75)
    parser.add_argument("--output-root", type=Path)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    root = Path(__file__).resolve().parents[1]
    output_root = args.output_root.resolve() if args.output_root else root / "artifacts/public-link-tests"
    out = output_root / uuid.uuid4().hex
    library = out / "library"
    for part in ("BGM", "SFX"):
        (library / part).mkdir(parents=True)
    process = subprocess.Popen(
        [str(exe), "--library", str(library)],
        env=dict(os.environ, LOCALAPPDATA=str(out / "profile")),
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    panel = 0
    results = []

    def owned(kind):
        found = []
        win32gui.EnumWindows(
            lambda window, _: found.append(window)
            if win32process.GetWindowThreadProcessId(window)[1] == process.pid
            and win32gui.GetClassName(window) == kind
            and win32gui.IsWindowVisible(window) else None, None,
        )
        return found[0] if found else 0

    def wait(condition, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("Owned app exited: " + str(process.returncode))
            value = condition()
            if value:
                return value
            time.sleep(0.04)
        raise TimeoutError("Dialog state timeout")

    def preparation_lines():
        return [line for path in (out / "profile").rglob("*.log")
                for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
                if "LINK_READY " in line]

    try:
        panel = wait(lambda: owned("MusicBoard.CompositionWindow"))
        time.sleep(0.4)
        win32gui.PostMessage(panel, win32con.WM_COMMAND, 20, 0)
        dialog = wait(lambda: owned("MusicBoard.LinkImport"))
        controls = {i: win32gui.GetDlgItem(dialog, i) for i in range(101, 108)}
        if not all(controls.values()):
            raise RuntimeError("Shipped link controls are missing")
        win32gui.SendMessage(controls[104], win32con.BM_SETCHECK, win32con.BST_UNCHECKED, 0)
        for name in args.case or CASES:
            print(json.dumps({"Starting": name}), flush=True)
            url = CASES[name]
            win32gui.SendMessage(controls[101], win32con.WM_SETTEXT, 0, url)
            win32gui.SendMessage(controls[102], win32con.WM_SETTEXT, 0, name)
            started = time.monotonic()
            earlier_preparations = set(preparation_lines())
            win32gui.SendMessage(controls[106], win32con.BM_CLICK, 0, 0)
            states = []
            timed_out = False
            while not win32gui.IsWindowEnabled(controls[106]):
                current = win32gui.GetWindowText(controls[105])
                if not states or states[-1] != current:
                    states.append(current)
                if time.monotonic() - started > args.timeout:
                    timed_out = True
                    win32gui.SendMessage(controls[107], win32con.BM_CLICK, 0, 0)
                    wait(lambda: win32gui.IsWindowEnabled(controls[106]), 5)
                    break
                if process.poll() is not None:
                    raise RuntimeError("Owned app exited during download")
                time.sleep(0.06)
            time.sleep(0.25)
            status = win32gui.GetWindowText(controls[105])
            files = list((library / "BGM" / name).glob("*"))
            audio = []
            for path in files:
                if not path.is_file():
                    continue
                probe = subprocess.run(
                    [str(exe.parent / "media-tools/ffprobe.exe"), "-v", "error",
                     "-show_entries", "format=duration:stream=codec_type,codec_name",
                     "-of", "json", str(path)],
                    capture_output=True, timeout=15, encoding="utf-8", errors="replace",
                )
                metadata = json.loads(probe.stdout) if probe.returncode == 0 else {}
                valid = any(s.get("codec_type") == "audio" for s in metadata.get("streams", []))
                audio.append({"Name": path.name, "Bytes": path.stat().st_size,
                              "SHA256": hashlib.sha256(path.read_bytes()).hexdigest(),
                              "AudioValid": valid, "Metadata": metadata})
            result = {"Provider": name, "Url": url, "Seconds": round(time.monotonic() - started, 2),
                      "TimedOut": timed_out, "Status": status, "States": states,
                      "Files": audio, "DownloadPassed": len(audio) == 1 and audio[0]["AudioValid"]}
            result["NativePlaybackPreparation"] = [line for line in preparation_lines() if line not in earlier_preparations]
            results.append(result)
            (out / "report.json").write_text(json.dumps({"ExecutableSHA256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                "Scope": "Real packaged Windows dialog; public Internet; private library; download and audio-container validation only",
                "Results": results}, ensure_ascii=False, indent=2), encoding="utf-8")
            print(json.dumps({key: result[key] for key in ("Provider", "Seconds", "TimedOut", "Status", "DownloadPassed")},
                             ensure_ascii=False), flush=True)
        print(json.dumps({"Report": str(out / "report.json"), "OwnedPid": process.pid}), flush=True)
    finally:
        if panel and process.poll() is None:
            win32gui.PostMessage(panel, win32con.WM_COMMAND, 3, 0)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)
        (out / "execution.json").write_text(json.dumps({"OwnedPid": process.pid,
            "ExitCode": process.returncode, "ProcessExited": process.poll() is not None,
            "CompletedCases": len(results), "RequestedCases": len(args.case or CASES)}, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
