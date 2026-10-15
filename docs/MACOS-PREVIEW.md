# MusicBoard macOS Preview 1

Native AppKit/SwiftUI and AVFoundation port; the existing Windows release is unchanged.
Minimum deployment target: macOS 13. Apple silicon and Intel are built separately.
This preview contains the library, search/categories, music, up to four effects,
loop, volume, fade/mix, floating orb, pinning, drag-and-drop and link imports.
It is not full Windows feature parity. Deck assignment modes, global hotkeys,
automatic installation of updates, all-source streaming and physical multi-monitor
acceptance remain outside this first Mac preview. The update menu opens its release page.

## Installation and permissions

Open the DMG and copy MusicBoard to Applications. A standard account can use its
own `~/Applications` folder without administrator rights. A system-wide installation
may require authorization depending on ownership and device management.
Quit from the MusicBoard menu before moving the application to Trash.
Preferences and downloads remain in `~/Library/Application Support/MusicBoard`.

Audio playback requires no microphone, camera, screen recording, Accessibility or
Apple Events/Automation permission. Select the library with the native folder picker.
The app reads selected library audio; downloaded media and preferences use the app's
own support folder. It does not read browser cookies or bundled user media.

The app has an ad-hoc signature only. No Developer ID certificate or Apple
notarization is included, so Gatekeeper can block the first launch. Follow
[Apple's official guidance](https://support.apple.com/102445); do not disable
system-wide security checks. Native CI launch is not a Finder/quarantine acceptance test.

## Sources and validation scope

The selected product icon is reused. The FFmpeg helper is compiled from exact
9.0.2 LGPL sources, without external/GPL libraries; its source archive, license and
build recipe are inside `Contents/Resources/Licenses`. yt-dlp 2026.08.19 is a
separate pinned universal executable with upstream license notices. Existing
FullTools single-track adapters are reused without reading account credentials.

Direct HTTP audio uses AVPlayer. Page links use yt-dlp; sources requiring additional
headers should use Download & Play in this preview. Downloads are converted to M4A
using bundled helpers and persist in the local catalog. Login, subscriptions,
regional restrictions and expired links remain provider constraints.

`tools/macos/verify.py` exercises real AVPlayer/decoder, UI state, pinning, Chinese
filenames, library integrity, transitions, preferences and authored HTTP online/offline
audio. `package.py` separately mounts the exact DMG, copies the app to an owned user
installation fixture, verifies every file, launches the installed app and removes
only that fixture while keeping its profile. It records `spctl` assessment, not a
claim of Apple notarization. Physical speakers, real user permissions on all macOS
versions and every public provider link are not covered by these checks.

## 中文说明

这是原生 Mac 测试版，保留主要的音乐与音效操作；尚未达到 Windows 版的全部功能。
打开 DMG，将程序拖入“应用程序”。没有管理员权限时，可以放入个人 `~/Applications`。
播放不申请麦克风、录屏或辅助功能权限；曲库通过文件夹选择器主动选择。
尚未经过 Apple 公证，首次打开可能需要在“系统设置 → 隐私与安全性”确认。
下载和设置保留在本机，卸载应用时默认保留。在线播放受来源限制；需要附加请求头的
来源在此测试版中使用“下载并播放”。
