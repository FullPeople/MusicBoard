# 音乐板 · 项目入口

产品、架构与验收的正本是 [docs/IMPLEMENTATION.zh-CN.md](docs/IMPLEMENTATION.zh-CN.md)。
交互与视觉的原型是 [design/mockup.html](design/mockup.html)，方案是 [design/DESIGN.md](design/DESIGN.md)。
当前状态以正本与工作树为准。

## 持续有效的边界

以下几条不是"这一阶段的要求"，而是长期有效、不随进度失效的约束。
任何改动如果与它们冲突，先改设计再来改代码。

### 1. 不用异常，失败必须被显式处理

所有可能失败的函数返回 `bool` 并填写 `Failure{code, context}`，并标注 `[[nodiscard]]`。

- **禁止静默回退。** 不许用默认值掩盖失败，不许用"关闭功能"隐藏失败，
  不许把"解不出来"当成"文件本来就没有"。
- **禁止空错误码。** 失败时 `code` 与 `context` 都必须有值，且 `context` 要含足够
  定位问题的信息（哪个文件、什么状态、Win32 错误几）。
- 最内层的失败原因优先：`Failure::set` 不覆盖已有内容。
- **静默纠正也算静默回退。** 把越界取值夹到合法区间是可以的，但必须留下一条说明
  （见设置存储的 `notes`），不能改完什么都不说。

这条边界的由来：Godot 原型里的静默失败正是要换掉的东西 ——
文件操作失败只写控制台、取消下载只把标志位置回 false、进度条永远不会动。
用户看到的是"点了没反应"。编译期强制处理，这类问题就没有藏身之处。

### 2. 路径只有一个 UTF-8 边界

Windows 上 `std::filesystem::path::string()` 走活动代码页（中文系统是 936/GBK）。
同一个中文文件名会得到两种字节序列：

```
path.string()   -> 19 字节（GBK）
path.u8string() -> 26 字节（UTF-8）
```

- 内部一律用 `std::filesystem::path` 与宽字符 Win32 API。
- 只在"给人看"或"序列化"的边界上转 UTF-8，且必须经过 `core/path_utf8.h` 里的函数。
- **不允许在别处直接调用 `path.string()`。**
- **绝不要写 `path / std::string` 或 `path / std::string_view`。** 那个重载走的是同一个
  活动代码页。实测的后果是：`create_category("战斗")` 返回成功、目录确实建出来了，
  但扫描器（走宽字符）找不到它 —— 用户看到的是"我建了分类，可它不出现"。
  要拼路径一律用 `library_ops.h` 的 `track_path_for`。
  **这个坑和下面两条是同一类**：都在"窄字符串 ↔ 宽路径"这一处，而且都不报错。
- **进程入口必须用 `wmain`。** MSVC 的 `char** argv` 同样走活动代码页，
  中文路径会被打乱。读环境变量同理，用 `GetEnvironmentVariableW` 而不是 `getenv`。
- 第三方库的文件 API 只要收窄字符路径，就一律不要用（见 stb_vorbis 的处理）。
- **凡是"用户输入的 UTF-8 字符串"，先验证它是合法 UTF-8**（`library_ops` 的
  `common_name_check` 做了这件事）。不先挡这一条，后面构造路径时才会失败，
  而那个失败更难解释。

### 3. 与时间有关的东西一律用绝对时间戳，不用增量累加

淡入淡出、进度、超时 —— 只要它本该由时间决定，就必须能从"现在几点"直接算出来。

- **禁止用"每帧累加"描述它们。** 原版就是这样栽的：打开文件夹对话框（`tree.paused`）时
  有一帧没跑到，音量就永远卡在半途。
- 实现见 `src/audio/voice_timeline.h`：`gain_at(now)` 是个纯函数，
  查询多少次、间隔多大，同一时刻的结果都一样（有专门的测试钉住这条）。

### 4. 扫描只 probe，预加载采用有界分层数据

2026-09-19 用户明确要求集中加载，并选择“短音效完整预载 + 长曲起播段与后台连续供音”。
本次有效设计见 [design/2026-09-19-orb-dj-preload.md](design/2026-09-19-orb-dj-preload.md)。
允许在独立、可取消、有真实进度的预载阶段解码；禁止把整个长曲库的 PCM 常驻内存。
下面的旧约束继续用于扫描器，不再用于否定已授权的独立预载阶段。

`probe`（只读格式与时长）与 `decode`（解成 PCM）是两个操作，不能合并。

- 扫描曲库只准用 `probe_audio_file`。
- 只有真正要播某一首时才准调 `decode_audio_file`。

实测依据：真实曲库 491 个文件 / 3.7 GB / 42.7 小时。
全部 probe 一遍耗时约 3 秒、工作集增量 **1–2 MB**（`library_scanner_tests` 每次都实测并断言
上限 64 MB）；若改成扫描阶段就取 PCM，常驻内存会是 GB 级。

**没有例外。** 扫描器里也不许"顺便"读一小段波形或封面 ——
那正是把 6 MB 变成 GB 级的开始。波形与封面按需另取，并且不进扫描结果。

### 5. 容器按内容判断，不按扩展名

扩展名是用户随手改的。一个名叫 `.mp3` 的 ogg 交给 Media Foundation 只会得到
`MF_E_BYTESOURCE_UNKNOWN_TYPE`，然后被误判成"文件坏了"。
一律先 `probe_container` 嗅探魔数，再决定交给哪个后端。

### 6. 每一个后端都要有对应测试，且要对着真实素材跑

- 测试的成败不该取决于某台机器上有没有素材：没有就 `SKIP`，不要 `FAIL`。
- 每个断言失败时要打出实际值（`check_eq` 的 detail），省一次复现。
- 新增能力要留一篇 `docs/validation/YYYY-MM-DD-*.md` 记录实测数据。

### 7. 不拿旧证据顶替当前验收

旧的日志、静态截图、内部 PASS 都不能代替当前真实运行的结果。
改了东西就重跑，并把新数据写进当次的验证记录。

## 任务路由

| 任务 | 阅读范围 |
| --- | --- |
| 视觉与交互 | [design/DESIGN.md](design/DESIGN.md) 与 [design/mockup.html](design/mockup.html) |
| 构建与依赖 | 本文件"构建约定"、`cmake/CompilerOptions.cmake` |
| 音频解码 | 正本的"音频方案"一节，以及 `docs/validation/2026-09-16-phase0-1-skeleton-decode.md` |
| 窗口、透明、DPI | 正本的"窗口方案"一节，以及 `docs/validation/2026-09-16-phase2-window-spike.md` |
| 悬浮球、圆形命中测试、拖动 | 正本的"窗口方案"一节，以及 `docs/validation/2026-09-16-phase3.5-orb-window.md` |
| 设置存储、单实例、崩溃转储 | 正本的"core 收尾"一节，以及 `docs/validation/2026-09-16-phase3-core-closeout.md` |
| 音频引擎、淡入淡出、播放控制 | 正本的"音频引擎"一节，以及 `docs/validation/2026-09-16-phase4-audio-engine.md` |
| 曲库扫描、分类、文件操作、导入 | 正本的"曲库层"一节，以及 `docs/validation/2026-09-16-phase5-library.md` |
| 波形包络、封面提取 | 正本的"曲库层"一节，以及 `docs/validation/2026-09-16-phase6a-library-data.md` |
| 界面命中测试、层次、滚动、封面缓存 | `docs/validation/2026-09-16-phase6b-ui-core.md`；布局数字以 `design/mockup.html` 为准 |
| 正式程序、交互行为与自检 | `docs/validation/2026-09-16-phase6b4-application.md`；自检入口 `musicboard.exe --selftest` |
| 安装、卸载、产品注册、打包 | `docs/validation/2026-09-16-phase7-install-package.md`；`src/install/`、`tools/package_release.ps1` |
| URL 导入、子进程、暂存区、下载进度、直链下载 | 正本的"曲库层"一节，以及 `docs/validation/2026-09-16-phase5b-url-import.md` |
| 新增一个音频后端 | 正本"音频方案"、`src/audio/decoder_backends.h`，并补 `decoder_tests` 用例 |

## 构建约定

- C++20、Win32、Visual Studio 2022、CMake 3.25+。
- 警告当错误（`MUSICBOARD_WARNINGS_AS_ERRORS=ON`）。刻意不用 `/Wall`：
  它会把系统头里的噪音也报出来。
- 全项目统一静态 CRT（`/MT`）。**不要逐目标设置** —— 一旦哪个目标漏了，
  就会出现 `/MT` 与 `/MD` 混用，表现为 `__imp_realloc` 这类"找不到 DLL 导入符号"的
  链接错误，报错位置离真正原因很远。实测被 stb_vorbis 咬过一次。
- 依赖要么锁到具体版本 + SHA256，要么是单文件公共领域代码（如 stb_vorbis）。

### 必须知道的编译开关

| 开关 | 不加会怎样 |
| --- | --- |
| `/utf-8` | MSVC 按 936 读源码，含中文的源文件直接编译不过，`u8""` 也解析失败 |
| `/Zc:__cplusplus` | `__cplusplus` 恒为 199711，所有 C++ 版本判断失效 |
| `/permissive-` | 关掉 MSVC 宽松模式，贴近标准 |
| `UNICODE` / `_UNICODE` | Win32 头里的 `IDC_ARROW`、`TEXT()` 会解析成窄字符版本，在 `LoadCursorW` 这类显式宽字符 API 上类型不匹配 |

### Windows 上的几个隐蔽坑

写在 `AGENTS.md` 里而不是只留在提交信息里，因为它们都**报错位置离原因很远**：

- **`small` 是宏。** `rpcndr.h` 把它定义成 `char`（16 位 Windows 遗留）。
  写 `ComPtr<IDWriteTextFormat> small;` 会报"类型后面接 char 是非法的"，完全指不到名字。
  判断特征：**报错提到的 token 你在源码里根本找不到。**
- **`constexpr D2D1_COLOR_F` 编不过。** `D2D1::ColorF` 的构造函数不是 `constexpr`，
  要用 C++17 的 `inline const`。
- **`CreateSolidColorBrush` 收引用不收指针。** 第二个参数传 `nullptr` 不行。
- **D2D1CreateFactory 只暴露收 `void**` 的原始重载**，模板形式在这个 SDK 配置下不可见。
- **`getenv` 与 `char** argv` 都走活动代码页。** 中文路径必须用 `wmain`
  与 `GetEnvironmentVariableW`，否则路径字节错乱而终端看着正常（两处错得刚好对上）。
  **这个坑犯过两次** —— 第二次是在新写的测试里。
- **`path / std::string` 同样走活动代码页**，而且它不报错：目录建出来了，
  只是名字被打乱，于是"建了分类却不显示"。（详见上面第 2 条边界。）
- **`windows.h` 的 `min`/`max` 宏会破坏 `std::min`/`std::max`/`std::clamp`**，
  报错是 `error C2589: "(" : "::" 右边的非法标记`，看不出跟宏有关。
  项目级已定义 `NOMINMAX`。
- **XAudio2 的 voice 接口不是 COM 引用计数的**，没有 `AddRef`/`Release`，
  必须用 `DestroyVoice()`。用 `ComPtr` 会编译不过。
- **`ComPtr` 与手动 `Release()` 不能并存**，那是双重释放。
- **调试测试里必须让输出不缓冲、且把 CRT 断言改写 stderr。** Debug CRT 断言失败会
  `abort()`，而重定向到文件时 stdout 是块缓冲的：崩溃前打印的每一条 PASS/FAIL 都会丢，
  同时断言默认弹一个模态对话框 —— 在没人看的自动化里就是永久挂起。
  合起来的效果是"现场什么都没有"。`tests/test_harness.h` 的 `Registry` 构造时做了这两件事。
- **`ADL` 会让自定义的 `is_symlink` 与 `std::filesystem::is_symlink` 撞车**（C2668 调用不明确），
  因为 `std::filesystem::path` 参数把标准库的那一个也带进了候选集。改个名字即可（`path_is_symlink`）。
- **绝不要写 `std::wstring(s.begin(), s.end())` 去把窄字符串变成宽字符串。**
  那是**逐字节加宽**：UTF-8 的"音乐板"（9 字节）会变成 9 个 Latin-1 字符。
  实测症状是注册表的 DisplayName 在"设置 → 应用"里显示成 `???￤?????`。
  必须经 `utf8_to_wide`。
- **测试里的期望值要独立构造，不要用实现自己的转换结果。**
  上面那个 bug 之所以没被单元测试抓住，就是因为断言两边用了同一种错法
  （都逐字节加宽）于是相等。**一条因为错误原因通过的断言比一条红的更危险。**
- **带中文的补丁脚本一律写成文件再跑，不要用 `python3 - <<'EOF'` 内联。**
  Git Bash 会把内联 heredoc 里的中文摸坏：同一段锚点文本在脚本文件里能匹配、
  内联时 `count()` 是 0。而报错看起来像"我把源码记错了"，非常费时间。
  写文件（用编辑器/Write 工具）再 `python3 tools/_patch.py`，顺便还能重跑。
- **状态结构跨帧复用时，"只在有数据的分支里赋值"一定会残留。**
  `state_` 是常驻成员，deck 的字段原来只在 `if (has_track)` 里写 ——
  放完/清空/切换时它们留着上一首的值：用户看到的是"清空了音波图还在"、
  "换曲后进度和波形没重置"。规矩：**每帧先整体清空再填**。
- **引擎里任何"相对某个基线"的量，换源时必须重置基线。**
  播放位置 = (SamplesPlayed − 基线) / 采样率 + seek 偏移，而 `play()` 从来没
  重置后两者 —— 于是换曲之后进度从上一首的残留开始算，甚至显示成满进度。
  另外 XAudio2 的 `SamplesPlayed` 是声部**一生**的累计值，Flush 不清零：
  基线要在 Start 之后重新读，不能想当然写 0。
- **去重集合与队列必须同生共死。** "只保留最新一个任务"这类优化里，
  被丢掉的任务如果不从"已排队"集合里摘掉，它们就永远排不进去、结果永远不出现 ——
  表现是"卡在正在载入"，而且日志里什么都没说。
- **后台批量任务必须给交互让路，而且要能"提上来"。** 整库预热几百个任务排在
  FIFO 里，悬停就会排在它们后面（等于把延迟调回来）。队列要分级；
  已经在低优先级队列里的 key，被交互请求命中时要**提升**而不是被去重挡掉。
- **测试脚本里的坐标必须按窗口矩形换算，不能假定窗口在 (0,0)。**
  标题栏拖动会把窗口挪走，位置还记在设置里 —— 探针会静默地什么都没测到。
  同理：卡片行之间只有几像素的缝，落进缝里 id 就是 -1，
  看起来像"命中区缺了"。行位置要**扫出来**，不要按行距推算。
- **命中区是"后加的在上层"。** 加"整块面板/台身当落点"这种大区域时，
  必须先登记它，再登记里面的按钮 —— 否则大区域会把按钮全盖住，
  症状是"按钮点不着，反倒触发了外面那层"。自检里立刻会红。
- **窗口里"没人再置脏"的东西要主动画。** 主面板靠 `dirty_` 驱动重画，
  而悬浮球没有自己的帧循环：只画一次的话，那一次可能发生在窗口首次呈现之前，
  内容会被丢掉（实测空闲时球面全黑、播起来才有画面）。现在头一秒内每帧都画。
- **抓 DirectComposition 合成的窗口有竞态。** `PrintWindow(PW_RENDERFULLCONTENT)`
  偶尔会抓到一张全黑图（正撞上它换合成表面）。用它当证据时必须重试几次取最好的
  那张 —— 第一版脚本就是单次抓取，于是得出了"悬浮球没画出来"的错误结论。
- **递归地"等某台在响"是不够的。** 上一首还在响时那种等待会立刻通过，
  于是"新的一首其实没起来"被掩盖。要等**指定的那一条**（比对应用自己的
  `bgm_index_` / `sfx_index_[deck]`）。一个因为错误原因通过的断言比一条红的更危险。
- **宽度别按 UTF-8 字节数估。** 中文一个字 3 字节，`12 * text.size()` 会把
  "从头播放"（4 字）算成 166px，真实约 60px —— 于是一行开关合计超出台面右边缘，
  最后那个被挤到看不见，而代码上看不出任何问题。要估就数**字符**（跳过
  `10xxxxxx` 续字节）。芯片那种要精确的直接用 DirectWrite 量（`measure_chips`）。
- **"只看某一处的变化"来决定要不要重画，一定会漏。** 面板原来只盯 BGM 的进度，
  于是音效台的唱片与波形**只在鼠标动的时候**才转（鼠标移动是唯一还会置脏的东西）。
  正确口径是"只要有东西在动就重画"：任一路在出声、或有过渡没走完。
- **动画的过渡量不能拿"从启动算起的秒数"当输入。** 唱针原来写
  `clamp(animation_time * 4, 0, 1)` —— 启动 0.25 秒后它就已经是 1 且永不再变，
  那段动画实际上从来没演过。过渡量要由**状态变化的时刻**（或逐帧推进的量）驱动。
- **`.ps1` 文件要带 UTF-8 BOM，否则 PowerShell 5.1 按 ANSI（936）读它。**
  症状不是"中文变乱码"这么明显，而是**花括号被吃掉**：UTF-8 的汉字是 3 字节，
  按 CP936 两字节一组解，错位后 `{` `}` `[` `]` `|` `\` 这些 0x40–0xFE 的
  ASCII 字符会被当成双字节字的尾字节吞掉，于是报 `意外的标记"}"`，
  而且**报的行号指向一段无关的注释**。纯英文注释的脚本一直没露这个马脚，
  中文注释一多就必炸。用 `python -c "open(p,'ab').write(b'ï»¿')"` 补 BOM。
- **`& $exe` 对 GUI 子系统的程序不等待。** PowerShell 只等控制台程序：
  `& musicboard.exe --version` 立刻返回、捕获不到任何输出、`$LASTEXITCODE`
  也不会被设置（在 `Set-StrictMode -Version Latest` 下直接变成硬报错）。
  程序从控制台子系统改成 GUI 子系统那天，打包脚本就是这么坏的。
  要拿退出码和输出必须 `Start-Process -Wait -PassThru -RedirectStandardOutput`。
- **`FindWindowW` 跨进程找不到窗口时，别急着怀疑类名。**
  同一进程里 `EnumWindows` + `GetClassNameW` 能拿到 `MusicBoard.CompositionWindow`，
  而 `FindWindowW` 一直返回 0。按 pid 枚举可靠得多。
  另外 P/Invoke 声明 `...W` 函数要写 `CharSet = CharSet.Unicode`，
  否则字符串按 ANSI 编组：`GetClassNameW` 写进去的宽字符串被读成 `"M"`（首字符 + NUL），
  于是"类名不匹配"看起来像"窗口不存在"。
- **`Process.MainWindowHandle` 不保证是你要的那个窗口。**
  这个进程里它给的是 184×184 的悬浮球，而主面板是 1256×788 ——
  拿球当面板的坐标系去发鼠标消息，得到的结论是"点哪儿都没反应"。
- **整条标题栏返回 `HTCAPTION` 会把标题栏上的按钮一起吞掉。**
  画得出来、点不着，而且不报错：Windows 把点击当成"拖动标题栏"，
  窗口过程压根收不到 `WM_LBUTTONDOWN`。按钮所在的矩形必须返回 `HTCLIENT`。
  这类"窗口过程层的命中测试"是交互自检**覆盖不到**的（自检直接调 `on_left_down`，
  绕过了窗口过程），只能靠真实鼠标消息 + 输入日志发现。
- **`IDCompositionVisual` 与 `IDCompositionVisual2` 都没有 `SetOpacity`**，
  它在 `IDCompositionVisual3` 上（翻 dcomp.h 确认过）。取不到就安静跳过 ——
  不透明度只是动效，不该因为它让面板出不来。
- **PowerShell 5.1 没有 `utf8NoBOM`**（而且它的 `UTF8` 会写 BOM，会破坏按行
  解析的清单文件），也**没有 `[System.IO.Path]::GetRelativePath`**（那是
  .NET Core 2.0+）。打包脚本一律 ASCII 源码，中文只放进包内的 UTF-8 txt。
  另外 `Write-Host` 的输出不走 `2>&1`；`+` 对两个 `[char]` 是整数加法不是拼接。
- **`\x01b` 是十六进制转义 0x1B，不是"0x01 后面接 b"**（最长匹配规则）。
  想测控制字符要写 `std::string("a\x01") + "b"`。
- **`CreateProcessW` 的命令行引号不是"前后加双引号"。** 反斜杠只有在双引号之前
  才需要翻倍转义；否则路径以 `\` 结尾时会把收尾引号转义掉，参数就粘在一起了。
  见 `src/library/process_runner.cpp` 的 `quote_argument`。
- **只杀子进程杀不掉孙进程。** yt-dlp 会自己起 ffmpeg。要连坐就用 Job Object
  （`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` + `TerminateJobObject`），
  并且 `CreateProcess` 时用 `CREATE_SUSPENDED` 先把 job 分配上再 `ResumeThread`。
  另外**管道读端必须设为不可继承**，否则子进程退出后管道永远不"关闭"。
- **子进程的 stdout 是文本模式**，CRT 会把 `\n` 写成 `\r\n`。
  在原始输出里直接 `find("...\n")` 永远找不到；要么按行拆开再比，
  要么把子进程的 stdout 设成二进制。
- **`path / std::string_view` 之外，`path_from_utf8` 也别当构造函数用**：
  它返回 `bool` 并且是出参形式，写成 `path(path_from_utf8(...))` 会被值语义
  悄悄编译过。抽个小包装函数（`path_of_utf8`）才看得清。
- **不要写 `check_eq(call(...), "...", failure.to_string())`。**
  C++ 不规定实参求值顺序，`failure.to_string()` 完全可能在调用**之前**就算好 ——
  于是失败时打印的是旧值（通常是空的），也就是最需要原因的时候它恰恰是空的。
  脚手架提供了 `check_invocation([&]{ return call(...); }, "...", failure)`，
  顺序由它保证。全项目统一改成了这个形式（18 处）。
- **COM 的初始化/反初始化要用 RAII 放在最前面。** 手写在 `return` 之前的话，
  COM 会先被反初始化，然后才轮到那些持有 COM 对象的局部变量析构 ——
  实测段错误一次。声明在最前面 → 析构在最后面。
- **`CoCreateInstance` 之前必须 `CoInitializeEx`。** 不初始化时返回
  `CO_E_NOTINITIALIZED`，症状是"某个模块初始化失败"加一句看不出所以然的 hr。
- **比较断言里的搜索串要核对目标文案。** 写过一次 `find("没有输出")` 去匹配
  "没有**任何**输出" —— 断言本身错了，而它看起来是会过的。
- **D2D 的"设备相关资源"不能跨 target 用，违反了也没有 HRESULT，直接 abort。**
  已经以三种面目踩到：几何/描边样式取自另一个工厂、封面位图跨 target、
  画刷跨 target（截图那条路是软件 target）。对策都是同一条：
  **资源在"真正要画的那张 target"上现建或按 target 缓存**
  （`PanelView::ensure_brush` / `target_supports_covers`）。
  文字格式是 DirectWrite 工厂建的，属于设备无关，可以共用。
- **D2D 的 device-independent 资源必须来自 render target 自己的工厂。**
  另建一个 `ID2D1Factory` 去 `CreateStrokeStyle` / `CreatePathGeometry`，
  再把结果交给另一个工厂的 render target 用 —— D2D **不返回 HRESULT，直接 abort()**。
  没有断言文本、没有异常，退出码 3。
  正确写法：`target.GetFactory(...)`（本文件的 `draw_waveform` 一直是这么写的）。
- **调用 `DrawGeometry` 之前必须释放 GeometrySink。** `Open` 的文档里写明，
  违反了同样是直接 abort。两条都是在做悬浮球进度弧时踩到的，
  排查过程记在 `docs/validation/2026-09-16-phase3.5-orb-window.md`。
- **纯虚函数不能在析构里调。** 基类析构期间虚表已经不指向子类，
  而纯虚函数没有定义 —— 症状是链接期"无法解析的外部符号"，看不出跟析构有关。
  要在析构里用子类提供的东西，就在构造时抄一份下来。
- **窗口是透明的，"压暗"不等于"降低 alpha"。** 降 alpha 会让元素变半透明，
  看起来像破了个洞而不是变暗。要把颜色往背景暗色上混，alpha 保持 1。
- **`WinHttpQueryHeaders` 的 `WINHTTP_QUERY_CUSTOM` 查不到某个头时只返回 FALSE**，
  不报错、不设最后错误。实测 `Content-Disposition` 就是这么被静默丢掉的
  （服务器明明给了文件名，界面用了 URL 里的哈希串）。
  改成一次拉 `WINHTTP_QUERY_RAW_HEADERS_CRLF` 自己按行找。
  这类"官方 API 静默不返回值"的地方只能靠测试发现 —— 解析函数本身全过，
  问题在它和 API 之间。
- **凡是"只在 pump 里更新快照"的对象，状态变更方法末尾也要搬一次快照。**
  `HttpDownload::cancel()` 改了原子状态却没更新 `status_`，
  于是"取消了，界面显示还在下"。

## 常用命令

```bash
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release

# 解码测试需要真实曲库；不设这个变量时相关用例会 SKIP 而不是失败
export MUSICBOARD_TEST_LIBRARY="D:/Desktop/音乐板/audio"
ctest --preset release --output-on-failure

# 曲库扫描（阶段 1 的验收证据）
./build/windows-x64/Release/scan_library.exe "D:/Desktop/音乐板/audio"
```
