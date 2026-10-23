# 音乐板 MusicBoard

[English](README.md) · [下载 Windows 开发版](https://github.com/FullPeople/MusicBoard/releases/tag/v0.3.6)

用于跑团场景的 Windows 原生音乐与音效控制板。BGM 与 SFX 独立控制，支持四台唱盘和三列 DJ 按钮。

- 常驻悬浮球：上部展开或立即隐藏面板，下部暂停/继续音乐。
- **淡入淡出**：覆盖起播、暂停、继续、停止以及非循环曲目的结尾。
- **混音**：使用约 2.5 秒的等功率交叉淡化切换音乐，旧曲渐弱时新曲渐强。
  关闭混音、保留淡入淡出时，旧曲先淡出，新曲再淡入；两个开关都关闭则直接切歌。
- DJ 音效默认单次播放，各按钮独立发声，右键停止。
- 分类、搜索、曲名前置标签、播放进度、独立音量和可配置快捷键。
- CN/EN 即时切换并记住语言，支持 DPI 缩放、置顶及仅显示 BGM 的紧凑面板。
- 短曲完整预载，长曲只预备前 2 秒并持续后台供音；波形按需加载。
- 链接导入支持直接在线播放，也可下载入库后离线播放。0.3.5 改善临时断网后的有限恢复；在线曲目使用有限缓冲，仅在当前会话保留。

## 使用

从 [Releases](https://github.com/FullPeople/MusicBoard/releases) 下载 Windows x64 单文件安装包。
0.3.6 安装向导提供许可阅读、安装内容、目录选择、桌面快捷方式选项和按实际工作量更新的进度。
如果下载便携 ZIP，完整解压后运行 `MusicBoardSetup.exe` 安装，也可直接启动 `MusicBoard.exe`。
不需要管理员权限或额外安装 VC++ 运行库；安装向导支持 `--lang en` 切换英文。

安装版启动后会自动检查更新，长期开着时也会继续定期检查，每天最多自动检查一次。
悬浮球或通知区域菜单中的“检查更新…”可立即手动检查。下载与安装仍需你确认，安装前请退出音乐板。
升级保留原安装目录、桌面快捷方式选择、曲库和设置。[安装验证记录](docs/validation/2026-10-23-installer-wizard.md)
单独说明测试范围；安装界面的实际流畅度和真实多屏切换仍由用户验收。

[0.3.5 Windows 开发版](https://github.com/FullPeople/MusicBoard/releases/tag/v0.3.5) 提供独立安装包。最终包的普通升级、启动与卸载 10 项，以及旧版经官网签名联网更新的 17 项实际检查通过，保留偏好和测试曲库。七个公开来源的 63 项实际播放检查通过，另有组件在 30 秒断网后的 WAV／MP3 续传检查；组件、完整应用和公开升级的范围分别记录。[验证记录](docs/NETWORK-VALIDATION-20261014.md) 保留成功与失败。网易和酷我样本仅为试听。需要登录、付费、区域限制、过期地址或无确定时长的来源可能无法使用；播放中长期中断、所有歌曲和 macOS 仍在开发。

首次启动选择曲库目录，例如：

```text
曲库/
  BGM/探索/音乐.mp3
  SFX/音效/效果.wav
```

软件不附带音乐素材。将音乐卡拖到唱片上播放，点击唱片暂停/继续；重复投放同一曲目会继续播放。
唱盘模式下右键音效卡，优先分配到空台；四台均占用时覆盖第一台。
Ctrl+Alt+1..4 快捷键仍然可用，可在悬浮球右键菜单中修改。

左下角折叠按钮用于只显示音乐；“显示全部”默认关闭，同时控制两列的“全部”分类。
曲库路径只读，右侧有更换和重新加载按钮。关闭面板仍保留悬浮球和播放，通过右键菜单退出程序。
设置与缓存位于 `%LOCALAPPDATA%\MusicBoard`，卸载保留曲库和用户数据。

## 构建

需要 Windows x64、Visual Studio 2022 的 C++ 桌面开发组件、Windows SDK 和 CMake 3.25+。

```powershell
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release --output-on-failure
```

输出为 `build/windows-x64/Release/musicboard.exe`。打包使用 `tools/package_release.ps1 -Version 0.3.6`，
默认包含安装与卸载验证。需要真实曲库的测试可设置 `MUSICBOARD_TEST_LIBRARY`；无设备/素材时对应测试跳过。
贡献说明见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

[MIT](LICENSE)，Copyright 2026 FullPeople。第三方组件另见
[THIRD_PARTY_NOTICES](packaging/THIRD_PARTY_NOTICES.txt)。
