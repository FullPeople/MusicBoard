MusicBoard
===============

A native Windows soundboard for tabletop sessions. No music or sound effects are included.
The link importer can stream supported HTTP audio without saving a library file.
Online entries last for the current session; source access restrictions still apply.

1. Run the single-file installer to review licenses and installation contents, choose a folder
   and set the desktop shortcut option. Administrator rights are not required.
   For the portable ZIP, extract the entire archive and run MusicBoardSetup.exe or MusicBoard.exe.
   English installer: MusicBoardInstaller.exe --lang en; package backend: MusicBoardSetup.exe --lang en.
2. Select a library containing BGM/<category>/<tracks> and SFX/<category>/<effects>.
3. Click EN next to the pin button for English; the selection is remembered.

Playback
--------
Drag music onto the main record. Click the record to pause/resume.
Fade provides audible transitions on start, pause, resume, stop and at a non-looping track's end.
Mix overlaps the outgoing and incoming music with a 2.5-second equal-power crossfade.
With Mix off and Fade on, the old track fades out before the new track fades in.
Both switches off gives an immediate cut. Dropping the same track again continues or resumes it.

Use four independent effect decks or switch to three-column DJ Pads.
Pads play once by default; right-click stops a pad. In Decks mode, right-click an effect card to
assign it to the first free deck, replacing the first deck when all are occupied.
Ctrl+Alt+1..4 still control effect decks; configure shortcuts in the orb context menu.

The orb stays visible. Its upper area instantly hides the open panel or opens it;
its lower area pauses/resumes music. Drag the orb to move it.
The pin controls always-on-top. Closing the panel keeps the application running;
use Exit in the orb or tray context menu to quit.

The lower-left fold button hides the effects panel. Show All is off by default and controls
whether All appears in both category bars. The path is read-only; use Change or Reload beside it.

Short tracks are preloaded completely; long tracks preload their first two seconds and stream
through bounded buffers. Startup and reload show real progress. Waveforms are generated on demand.

Settings and caches are stored under %LOCALAPPDATA%\MusicBoard.
Uninstalling keeps your library and user data. The portable executable also uses this settings folder.
Installed copies check for updates after startup and continue checking while running, at most once
per day automatically. Check for updates in the orb/tray menu also provides an explicit check.
Download and installation require confirmation. Exit MusicBoard before installing an update.

Source: https://github.com/FullPeople/MusicBoard
License: MIT. See LICENSE.txt and THIRD_PARTY_NOTICES.txt.
