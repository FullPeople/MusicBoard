# MusicBoard

[简体中文](README.zh-CN.md) · [Download for Windows](https://github.com/FullPeople/MusicBoard/releases/latest)

A native Windows music and sound-effects board for tabletop sessions. Keep background music
on one deck and trigger independent effects from four decks or a grid of DJ pads.

- A persistent record orb opens or instantly hides the main panel and pauses/resumes music.
- **Fade** smoothly starts, pauses, resumes and stops playback, and fades non-looping track endings.
- **Mix** transitions between music tracks with a 2.5-second equal-power crossfade. With Mix off
  and Fade on, the outgoing track fades out before the incoming track fades in.
- DJ pads play once by default and can sound simultaneously; right-click a pad to stop it.
- Search and category filters, title tags, playback progress, independent volumes and global shortcuts.
- CN/EN switching, per-monitor DPI support, always-on-top, and a compact music-only panel.
- Short audio is preloaded in full; long tracks preload their first two seconds and stream through
  bounded buffers. Loading progress is visible and waveform extraction stays on demand.
- Version 0.3.3 offers direct online playback in the link-import dialog, alongside download for
  offline playback. Online entries use bounded buffers and last for the current session.

## Download and use

Download the Windows x64 ZIP from [Releases](https://github.com/FullPeople/MusicBoard/releases).
Extract the whole archive, then run `MusicBoardSetup.exe` to install for the current user,
or launch `MusicBoard.exe` directly. Choose **EN** next to the pin button for English.
No administrator rights or Visual C++ runtime installation is required.

The [0.3.3 Windows development release](https://github.com/FullPeople/MusicBoard/releases/tag/v0.3.3)
also provides a single-file installer. Actual ordinary upgrade/uninstall checks (7) and signed
public HTTPS update checks (17) passed with generated fixtures and preserved settings. Generated
HLS VOD and native link-import checks passed locally. More public platform links, slow networks,
interruption recovery and macOS remain under development; login, region restrictions and expired
or indefinite-duration sources may prevent playback.

Choose a local library with this structure:

```text
MyLibrary/
  BGM/
    Exploration/
      Track.mp3
  SFX/
    Effects/
      Effect.wav
```

You can also start with `MusicBoard.exe --library "D:\MyLibrary"`.
The application contains no music or sound effects. Use audio you have permission to use.

Drag a music card onto the record. Click the record to pause/resume. Dropping the same track
again continues it. For effects in Decks mode, right-click a card to fill the next free deck;
when all four are occupied, the first is replaced. Switch to DJ Pads for one-shot buttons.

Use the lower-left button to hide the effects panel. **Show All**, off by default, controls
the All category in both libraries. Library paths are read-only; **Change** and **Reload** sit beside them.
Closing the panel keeps the orb and audio available. Choose **Exit** in the orb or tray menu to quit.
Settings and caches are stored in `%LOCALAPPDATA%\MusicBoard`; uninstalling preserves your user data.

## Build

Windows x64, Visual Studio 2022 with Desktop development with C++, a Windows SDK, and CMake 3.25+.

```powershell
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release --output-on-failure
```

The main binary is `build/windows-x64/Release/musicboard.exe`. Package it with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/package_release.ps1 -Version 0.2.0
```

The source uses C++20, Win32, Direct2D/DirectWrite/DirectComposition and XAudio2.
Windows Media Foundation and the bundled `stb_vorbis` decoder handle audio decoding.
See [Contributing](CONTRIBUTING.md) for validation and implementation conventions.
Some tests need an audio device or your own music library; unavailable fixtures are reported as SKIP.
The optional WASAPI output test verifies generated tones without saving desktop audio.

## License

[MIT](LICENSE), copyright 2026 FullPeople. Third-party terms are listed in
[THIRD_PARTY_NOTICES](packaging/THIRD_PARTY_NOTICES.txt).
