# 0.2.0 GitHub 发布回执

用户确认：当前 GitHub 账号下公开仓库 MusicBoard，采用 MIT 许可证。

- 仓库：https://github.com/FullPeople/MusicBoard
- 公开可见性：PUBLIC；GitHub 识别许可证：MIT。
- 默认分支：main。
- 发布提交：`67072dd8012f00915ca9824ae2a54dabb3198f25`。
- 正式版本：https://github.com/FullPeople/MusicBoard/releases/tag/v0.2.0
- GitHub 独立构建、测试、打包：全部通过。
- 最终运行：https://github.com/FullPeople/MusicBoard/actions/runs/35463452407
- 发布包：`dist/MusicBoard-0.2.0-windows-x64.zip`，761103 字节。
- GitHub 返回的资产 SHA256 与本地一致：
  `f06cca4eb4d36d7c2bb732e93bf85f90a21352c108c73f4e6a95d89d9f7a3616`。
- ZIP 与独立 `.sha256` 校验文件已上传，版本为正式发布，非草稿、非预发布。

源码整理为独立的本地公开工作副本 `build/open-source-0.2.0`，有自己的 Git 历史和
`origin`。159 个导出文件与本工作区逐字节核对，另提供公开贡献者 AGENTS.md。
未上传曲库、缓存、用户设置、运行截图或原仓库的私有历史。
原工作树的已有修改完整保留；不要把本目录的私有历史直接推到公开仓库。

首轮 GitHub 编译成功但 core_tests 在旧版中文代码页演示断言中崩溃；修正了测试中的
窄路径转换，增加独立 UTF-8 及代理对断言，本地复测通过后推送。最终运行完全通过。
该修正仅影响测试，不改变已做真实音频输出与安装验证的发布程序。

本地证据：`build/release-0.2.0-github-actions-full.log`、
`build/release-0.2.0-package-integrity.json`、`build/public-source-manifest.json`。
功能验收与硬件限制见 [0.2.0 验证记录](2026-09-20-release-0.2.0.md)。
