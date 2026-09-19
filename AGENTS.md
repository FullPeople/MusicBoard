# MusicBoard contributor instructions

Read README.md and CONTRIBUTING.md before changing the project.
Current playback/UI decisions: design/2026-09-20-mix-localization.md.
Current validation: docs/validation/2026-09-20-release-0.2.0.md.

- Use C++20, wide Win32 paths and the shared UTF-8 conversion boundary.
- Check fallible operations and report Failure with a useful code and context. Never hide failures.
- Compute audio fades from absolute timestamps; keep source-buffer ownership valid until DestroyVoice.
- Scanning probes metadata. Preloading is a separate bounded stage: complete short audio and heads of long audio.
- Keep rendering and input in matching per-window DPI coordinates. Fixed 96-DPI layouts are scaled once.
- Keep UI language pairs complete while preserving user track names and categories.
- Run relevant tests and document current evidence. Simulated DPI notifications do not prove a real mixed-DPI desktop.
- Keep /utf-8, /Zc:__cplusplus, /permissive-, UNICODE, NOMINMAX and project-wide static CRT consistent.
- Never include libraries, caches, credentials or desktop recordings in the repository.
