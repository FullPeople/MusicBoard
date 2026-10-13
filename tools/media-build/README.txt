MusicBoard source-matched Windows audio conversion runtime

The runtime consists of FFmpeg 9.0.2 under LGPL 2.1 or later, and LAME 4.0
under its included license. The original source archives, FFmpeg release
signature and public key, Debian source checksums, and exact build record
are included in MusicBoard-media-corresponding-source.zip.

To rebuild, extract the source ZIP into an empty directory. Install Python
3.12+, Visual Studio 2022 C++ tools, CMake 3.25+ and Git for Windows, then run:

  python tools/build_media_runtime.py --configure

The recipe uses MSVC x64 with a static CRT and shared FFmpeg libraries. The
source archives are reused from the source ZIP. Only isolated build tools
are downloaded to .cache/minimal-media; no system packages are installed.
The pinned MSYS2 bootstrap uses the dated 20260927 upstream archive and fails
closed if its digest changes. You can also supply an existing matching MSYS2
environment containing bash, sh and make with --msys-dir <msys64-directory>.
No downloaded binary is accepted after a digest mismatch.

All default native decoders, demuxers and input protocols remain available.
Schannel provides Windows TLS. Output encoders include MP3 via LAME, PCM,
AAC and FLAC. External libraries are not autodetected. GPL and nonfree
components are not enabled. The application invokes this runtime as a
separate process, and users may replace it with a compatible build.

Windows build adaptations:
- The unmodified LAME encoder sources are selected from Makefile.am. Its
  MSVC configuration is generated with an eight-byte long double on x64.
- msvc-cl normalizes localized compiler banner and include-prefix text,
  delegates all parameters to cl.exe, and preserves its failure status.
- msvc-windres derives from FFmpeg compat/windows/mswindres and adds the
  resource compiler option -c65001 for UTF-8 configuration text. Its original
  source is in the supplied FFmpeg archive and is covered by LGPL 2.1+.

The exact URLs, source hashes, configure options, build-tool records and
runtime hashes are in provenance.json. No music, user documents, account
cookies or signing keys are included.
