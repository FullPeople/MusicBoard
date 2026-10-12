# 2026-09-20 十二项界面与唱片交互验收

依据用户本轮十二项要求以及 [界面增补设计](../../design/2026-09-20-interface-refinement.md)。
保留前轮分层预载、连续供音及悬浮球合成修复。本轮未修改参考项目或真实音频文件。

## 当前交付

可执行文件：`build/windows-x64/Release/musicboard.exe`，1,074,688 字节。

SHA256：`D67B5894220F9B6B51DDB45E4D334E985D12868297D464A44ED9675185479DAD`。

| 用户要求 | 当前行为与验证 |
| --- | --- |
| 启动面板开启 | 加载完成保持面板可见，球同时常驻；使用隔离配置启动当前 EXE 实测 |
| 曲库地址与按钮 | 地址只读无命中区；右侧更换曲库、重新加载分别命中；真实目录选择器打开并取消，重载成功 |
| 置顶图钉 | 图钉、收起、关闭从左到右；SVG 同源矢量图标，选中竖直、关闭倾斜；真实 WM_NCHITTEST 返回 HTCLIENT |
| DJ 密度与进度 | 每行三张、卡高 62 DIP，无重复操作说明；三张按钮分别提交独立声部，颜色遮罩按实际播放位置从左向右铺开 |
| 曲名标签 | 连续前置 ()、（）、[]、{}、【】解析为淡色标签；卡片标题去掉这些前缀，下行不再重复目录；搜索仍匹配完整原名 |
| 折叠音效 | 左下角切换完整双栏与 BGM 窄窗，实际宽度 1256→746→1256 DIP；隐藏 SFX 几何及命中区，音效继续播放 |
| 半速唱片 | BGM、四台 SFX、球稳定转速统一为 13 秒一圈；独立数值断言验证一圈增量 |
| 唱片/球悬停与启停 | 只有有曲目的唱片圆内使用手形、变暗并显示暂停/播放；球高度 70/30 水平分区，上部打开、下部暂停/继续 BGM；启停 280ms 速度和唱针过渡 |
| 可见搜索输入 | 两侧输入区明确边框，I 型鼠标；聚焦和每次编辑重置光标闪烁起点；正式窗口截图检测到输入后的垂直光标 |
| 无缝分类按钮 | 每行相连无横向间隔，只首项左角与末项右角圆；按实测文字宽度换行，窄窗重新排布 |
| 默认黑胶高光 | 倾斜双扇形反射，随角度旋转；取消压扁椭圆高光 |
| 球右键菜单 | 暂停/继续、中止播放、播放最近、快捷键、隐藏/显示面板、退出程序；当前 Win32 菜单实际六项，二级列表最多五首 |

“播放最近”按本次启动内 BGM 成功开始播放的次数降序；同次数最近播放优先。
暂停后继续不算一次新播放。最近菜单只显示曲名，点选重新播放；重新加载保留计数，
更换曲库后只列出当前库仍可用曲目。菜单暂停和中止针对 BGM。

长标签为循环、淡入淡出、从头播放预留空间，避免按钮被挤掉或命中错位。
启停角度使用绝对时间积分，暂停途中再次继续不会跳角；唱针方向也已纠正，播放时向盘内落下。
曲目只有括号、括号不完整、标题中部含括号时保留可读标题，不改变磁盘文件名。

## 当前验证证据

- Release 构建，警告视为错误：成功。日志 `build/refine-build7.log`。
- 完整 CTest：**27/27**，44.80 秒，真实素材变量已设置。日志 `build/refine-ctest-final.log`。
- 正式程序 `--selftest`：**195/195，0 跳过、0 失败**；退出码 0，外部调试器未捕获异常。
  日志 `build/refine-selftest-final/stdout.txt`、`build/refine-selftest-final.log`。
- 正式桌面窗口：**29/29**。通过 PID/窗口类定位，按窗口坐标和 DPI 换算。
  包括真实鼠标光标查询、标题非客户区命中、DJ 独立声部、球暂停继续、收起展开、尺寸变化、
  原生目录对话框、重载和正常退出。日志 `build/refine-native5.log`、`build/refine-native5/checks.json`。
- 新增纯逻辑覆盖标签解析、重复标签、未闭合括号、13 秒转速、停止后角度不漂移、暂停/恢复连续性、
  三列卡片、分行无缝分类、窄窗按钮布局、长标签不挤控制以及拖动卡片尺寸一致。
- `git diff --check`：通过。

已查看当前程序截图：

- [默认面板与黑胶高光](../../build/refine-native5/01-open.png)
- [DJ 三列和唱片悬停](../../build/refine-native5/04-disc-hover.png)
- [曲名标签与搜索光标](../../build/refine-native5/05-search.png)
- [悬浮球分区](../../build/refine-native5/07-orb-top.png)
- [暂停后的球](../../build/refine-native5/08-orb-paused.png)
- [BGM 窄窗](../../build/refine-native5/10-folded.png)
- [六项原生菜单](../../build/refine-native5/11-context-menu.png)

## 加载回归

当前相同 EXE，真实曲库 491 文件。使用隔离 LOCALAPPDATA，未改用户设置和真实曲库。
“无预载缓存”表示应用缓存目录为空，未清空 Windows 文件缓存，不等同重启系统后的磁盘冷启动。

| 场景 | 加载事务 | 进程启动/点击至就绪 | 缓存命中 |
| --- | ---: | ---: | ---: |
| 当前版本无预载缓存启动 | 5.123 秒 | 5.297 秒 | 0/491 |
| 当前版本复用缓存启动 | 2.120 秒 | 2.328 秒 | 491/491 |
| 底栏重新加载 | 1.857 秒 | 1.906 秒 | 491/491 |

日志：`build/refine-cold-final.log`、`build/refine-native5.log`。
共享预载 PCM **178.30 MiB**，无缓存启动进程峰值工作集 **276.08 MiB**；缓存启动峰值 **244.88 MiB**。
所有准备事务 warnings=0。无整库完整 PCM、完整波形计算回归。

## 重跑

```powershell
cmake --build --preset release --parallel 3
$env:MUSICBOARD_TEST_LIBRARY='D:/Desktop/音乐板/audio'
ctest --preset release --output-on-failure
python tools/debug_native.py --exe build/windows-x64/Release/musicboard.exe --library 'D:/Desktop/音乐板/audio' --out build/refine-selftest-new
python tools/verify_interface_runtime.py --exe build/windows-x64/Release/musicboard.exe --library 'D:/Desktop/音乐板/audio' --out build/refine-native-new
```

涉及全局快捷键的 CTest、程序自检和桌面探针应顺序运行。

验收边界：桌面检查是自动发送 Win32 消息及查询实际光标/截图，运行于本机 100% DPI；
未把它表述为真人完整鼠标、中文 IME 或跨显示器不同 DPI 的体验验收。
本轮没有测扬声器端到端的听感延迟，也没有重新声称完成此前未完成的产品功能。
旧 `dist` 安装包未更新，当前入口是上述 Release EXE。
