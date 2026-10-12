# Contributing

Build with Visual Studio 2022 (Desktop development with C++ and a Windows SDK) and CMake 3.25+.

```powershell
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release --output-on-failure
```

Tests requiring audio devices or a local music library report SKIP when those are unavailable.
To exercise real decoders, set `MUSICBOARD_TEST_LIBRARY` to your own BGM/SFX library before testing.
Interactive and WASAPI output checks require a normal Windows desktop; see `docs/validation`.
The optional Python output check uses NumPy and PyAudioWPatch. These are test dependencies only.

Keep failure handling explicit with `Failure` and checked return values. Use wide Windows paths
internally and `core/path_utf8.h` at UTF-8 boundaries. Audio fades must use absolute timestamps.
Library scanning probes metadata; layered playback preloading is a separate, bounded stage.
Both display and input must use the same per-window DPI. Translate authored UI text with paired
CN/EN strings, without translating library filenames or categories.

Include a short description of the user-visible problem and relevant tests with a change.
For click-offset reports, include Windows version, monitor scaling and whether the window was
moved between monitors. Do not attach your music collection or private settings to public issues.

Contributions are provided under the project's MIT license.
