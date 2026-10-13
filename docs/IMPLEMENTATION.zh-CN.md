# 音乐板 · 实现与验收正本

0.3.2 本地候选新增链接导入、试听标示、后台可取消入库、签名更新器及较小的源码构建媒体组件。当前事实见 [媒体组件与安装候选](validation/2026-10-13-media-runtime.md)、[后台入库](validation/2026-10-13-background-publication.md) 和 [公开链接验证](validation/2026-10-13-public-links.md)。候选尚未公开发行；独立在线模式、完整在线更新流程和 macOS 仍待完成。下方 0.2.0 为历史公开版本记录。

0.2.0 当前音频及语言以 [混音与中英切换](../design/2026-09-20-mix-localization.md) 为准。
双声部交叉淡化已接入真实播放链路；普通淡化覆盖起播、暂停、继续、停止及自然曲尾。
删除从头播放开关；增加 CN|EN 无缝语言选择并持久化，SFX 台不再绘制数字与快捷键角标。
设置 schema 7。源码已发布到 [FullPeople/MusicBoard](https://github.com/FullPeople/MusicBoard)（公开、MIT）。
本次验收及包校验见 [0.2.0 发布验证](validation/2026-09-20-release-0.2.0.md)。
GitHub 构建、测试、打包已通过，正式版本与源码发布见 [发布回执](validation/2026-09-20-github-publication.md)。
公开仓库的本地工作副本位于 `build/open-source-0.2.0`；原工作树与私有历史保留。

0.1.2 本轮修正：主面板的物理鼠标坐标按本窗口当前 DPI 转为逻辑坐标，保留小数；
清单在进程启动时声明 PerMonitorV2，不能把任何 ERROR_ACCESS_DENIED 都当成正确感知级别。
跨 DPI 变化取消旧手势并刷新悬停坐标。悬浮球上部关闭在点击回调内 SW_HIDE，快速重新打开
不会被旧收起计时器隐藏。底部路径右对齐，新增默认关闭并持久化的“显示全部”开关，
同时控制两列分类标签；设置 schema 6。具体分类保留，隐藏当前选中的全部时切换第一项分类。
新证据见 [0.1.2 DPI 与底栏修正](validation/2026-09-20-release-0.1.2.md)。

当前交互按 [2026-09-20 十二项界面增补](../design/2026-09-20-interface-refinement.md) 实现：
启动打开面板；路径只读与底栏换库/重载；SVG 同源图钉左移；DJ 三列进度卡；曲名标签；
可折叠 SFX；唱片半速及启停唱针；圆内悬停和球 70/30 分区；可见搜索光标；分行无缝分类；
扇形高光；六项球菜单与本次 BGM 次数排名。设置 schema 5 保存折叠状态。
0.1.1 打包前追加：置顶图钉状态与设置同步、球上部开关面板、标签/格式居中、
唱盘/DJ 无缝切换、音效库右键优先空台且满台覆盖第 1 台。
新证据见 [0.1.1 修正与打包](validation/2026-09-20-release-0.1.1.md)，替代旧记录中对应的启动、球和按钮位置。

已实现用户确认的 [常驻球 / DJ / 分层预加载](../design/2026-09-19-orb-dj-preload.md)。
独立预载阶段现在允许解码完整短曲和长曲起播段；扫描器仍只 probe。
长曲前 2 秒进入共享 PCM，短曲不超过 30 秒完整预载；播放使用独立线程和有界供音队列。
缓存检查源路径、大小、修改时间及缓存 SHA256，不再扫描整库 PCM 或完整波形。
OGG 采用宽字符文件映射，避免探测和起播时整份复制压缩数据。
本次实测与限制见 [2026-09-20 验收](validation/2026-09-20-orb-dj-preload.md)。

本文件是产品、架构与验收的**单一事实源**。`AGENTS.md` 是入口与任务路由，
`design/` 下是视觉与交互方案，本文件是技术上的正本。

当前阶段：**2026-09-20 混音、淡化、本地化与 0.2.0 开源发布。** 阶段 0–7 的历史记录描述各层落地，
不能等同于当前完整产品验收。交叉淡化已实现并完成输出测量，替代
[早期审查记录](validation/2026-09-19-runtime-audit.md) 中对应的未完成项。
当前 `dist` 的 0.2.0 安装包已完成安装与卸载验证。

**2026-09-19 审查**：修复每帧创建省略号对象导致退出栈溢出、音频请求丢失与取消失效、
多台位共享曲目、换格式仍复用旧声部、seek 后只循环部分曲目、快捷键取错列、暂停清空与
输入捕获等问题。当时启动恢复只 probe；此后用户明确要求加入上述独立分层预载。
首次启动提供曲库选择器。Release CTest 26/26，正式程序自检 154/154 且退出码 0，
真实窗口消息复核 14/14；不替代真人听感、硬件鼠标与完整 DPI 矩阵验收。

**2026-09-17 第四轮**：修三个真 bug —— 换曲后播放位置从上一首的残留开始算
（进度错/满进度）、后台音频队列的去重集合泄漏（大量音乐永远卡在"正在载入"）、
state_ 跨帧复用导致清空/换曲后波形不重置；另修悬浮球闪烁与封面、音量条命中区、
音效暂停压暗、拖拽幽灵宽度、拖拽时台内按钮拦截落点、清空按钮压住角标；
当时新增**启动后台预热整库**（已于 2026-09-19 撤回）与悬浮球点击开关。见
[原型对齐第四轮](validation/2026-09-17-prototype-alignment-round4.md)。

**2026-09-17 第三轮**：按反馈修滚轮（两列独立 + 首次即可滚）、拖拽幽灵（卡片本身、
从抓取点起）、音量条错位、音效暂停不压暗、整块台身作为落点、上一首/下一首挪到
BGM 台音波图下方、列头清空按钮、托盘"退出"真退出、放置音乐不再冻结（后台解码）、
以及**悬浮球从来没被画出来**（抽出 ui/orb_view）。见
[原型对齐第三轮](validation/2026-09-17-prototype-alignment-round3.md)。

**2026-09-17 第二轮**：按用户的逐条反馈把交互与视觉向原型对齐 ——
悬停延迟（包络改后台提取）、拖拽幽灵、跳转位置与循环区间、音效暂停语义、
循环开关作用到引擎、BGM 台三个开关的命中区、底栏上一首/下一首、右上角三个键、
唱片的圆形封面裁剪与中心轴孔、唱针动画、音量条拖拽与加长、平滑滚动、
卡片去掉格式徽章、芯片换行与高亮不被裁。见
[原型对齐第二轮](validation/2026-09-17-prototype-alignment-round2.md)。

**第一轮（2026-09-17）修的是"能装能跑但不好用"**：右列音效库压根没被载入、
标题栏三个按钮点不到、程序被编成控制台子系统（常驻黑窗口 + 乱码）、
悬停没有过渡动效。交付形态也从控制台子系统改成了 GUI 子系统。
过程、证据与踩到的坑见
[2026-09-17 两列曲库 / 交互动效 / 标题栏按钮 / 单实例](validation/2026-09-17-two-columns-interaction-animation.md)。

---

## 1. 产品定位

跑团时挂在桌面上的音乐控制板。两个形态：常驻的**悬浮球**与会话用的**主面板**。

完整体验以 [design/mockup.html](../design/mockup.html) 为准 —— 那是一份可交互的
单文件离线原型，已经把两态切换、唱片台读法、拖放播放、悬停预览、BGM/SFX 独立
总线等全部交互做出来了。技术方案见 [design/DESIGN.md](../design/DESIGN.md)。

**本文件只覆盖已经落地的部分**，未落地的写在"后续阶段"里，不混进来。

---

## 2. 为什么不是 Godot

原型（Godot 4.7）已经跑通了交互，换原生要解决的是它做不到或做不好的事：

| Godot 版的现象 | 根因 |
| --- | --- |
| 透明窗口配了却没生效，系统标题栏与自绘标题栏同时在 | Godot 的透明无边框窗口在 Windows 上是半成品 |
| 整个曲库在启动时被读进内存 | 扫描阶段就对每个文件 `get_buffer(get_length())` |
| 淡入淡出在打开文件夹对话框时会卡住 | 靠每帧 `delta` 累加，`tree.paused` 期间停止处理 |
| 进度条是假的、取消导入不取消、"从头播放"开关无效果 | 调用阻塞式 `OS.execute`，且失败只写控制台 |
| 导出包 6.4 GB | `export_filter="all_resources"` 把整套音乐库打进了 exe |

收益在窗口/合成/音频控制；成本在 UI 控件（Godot 白送的容器、主题、拖放、
CJK 字体回退都要自己写）。这个取舍已经和用户确认过。

---

## 3. 真实素材

一切性能与兼容性判断都基于这份真实曲库，不用假想数据：

| 项 | 值 |
| --- | --- |
| 位置 | `D:\Desktop\音乐板\audio`（可由 `MUSICBOARD_TEST_LIBRARY` 覆盖） |
| 文件数 | 491 |
| 体积 | 3.7 GB |
| 总时长 | 42.7 小时 |
| 容器分布 | mp3 350 / ogg 137 / wav 4 |
| 目录结构 | `BGM/<分类>/` 与 `SFX/<分类>/`，分类名是中文 |
| 文件名 | 中文 + 全角符号（`｜`）+ 方括号 + 空格，例如 `[低][休整] 和平 水边 轻松｜Fast Food.mp3` |

**文件名这一项不是花边，它是本项目的技术命门。** 详见 `AGENTS.md` 的第 2 条边界。

---

## 4. 构建约定

见 `AGENTS.md` 的"构建约定"一节。三个关键开关（`/utf-8`、`/Zc:__cplusplus`、
`/permissive-`）各自对应一个实测踩过的坑，那里写了不加会怎样。

分层静态库：

```
musicboard_core             路径 UTF-8 边界、错误契约、文件读取、日志、
                            设置存储、单实例守卫、崩溃转储
  ├── musicboard_audio      容器嗅探 + 解码契约 + 两个后端 + XAudio2 引擎 + 波形包络
  │     └── musicboard_stb_vorbis（第三方，不参与本项目的警告标准）
  ├── musicboard_library    曲库扫描 + 文件操作与名字校验 + 导入事务 + 封面提取
  ├── musicboard_ui         界面交互内核 + 封面位图缓存 + 面板布局 + 面板绘制
  ├── musicboard_install    安装事务（校验 / 日志 / 回滚）+ 产品注册（注册表 / 快捷方式）
  └── musicboard_platform   透明窗口基类 + 主面板 + 悬浮球 + 全局快捷键 + per-monitor DPI
```

依赖方向单向向上，由 CMake 目标在编译期保证。

---

## 5. 音频方案

### 5.1 probe 与 decode 必须分开

这是整个音频设计里最重要的一条，也是它和 Godot 版最本质的区别。

| 操作 | 做什么 | 什么时候用 |
| --- | --- | --- |
| `probe_audio_file` | 只读格式与时长，**不解码** | 扫描曲库 |
| `decode_audio_file` | 解成 PCM16 交错样本 | 真正要播某一首时 |

**实测数据（阶段 1 验收）：**

| 指标 | 数值 |
| --- | --- |
| 扫描 491 个文件（3.7 GB） | 3.6 秒（平均 7.33 ms/文件） |
| 内存占用增量 | **6 MB** |
| 成功率 | 491 / 491 |

Godot 版在扫描阶段就取 PCM，同样这份曲库需要 GB 级常驻内存。
**6 MB 与 3.7 GB 之间差的是接口设计，不是优化。**
所以这条写进了 `AGENTS.md` 的持续边界，不允许日后被"顺手合并"掉。

### 5.2 两个后端

| 容器 | 后端 | 依据 |
| --- | --- | --- |
| wav / mp3 / m4a / wma / flac | **Media Foundation** | 系统自带，零依赖 |
| **ogg** | **stb_vorbis** | MF 在实测机器上解不了，见下 |

MF 实测（`tools/scan_library` 与阶段 1 探针的结论）：

```
[低][休整] 和平 水边 轻松｜Fast Food.mp3
  OK    [fromURL] native=MP3 -> PCM 44100Hz/2ch/16bit dur=302.03s firstFrame=yes
小鼠群.wav
  OK    [fromURL] native=PCM -> PCM 44100Hz/2ch/16bit dur=27.43s firstFrame=yes
[低][宁静] 平和 温暖.ogg
  FAIL  fromByteStream hr=0xC00D36C4      # MF_E_BYTESOURCE_UNKNOWN_TYPE
```

抽 8 个 ogg 是 0 通过 / 8 失败。不用 stb_vorbis 就会丢掉 137/491（28%）的曲目。

stb_vorbis 只走**内存解码**，编译时定义 `STB_VORBIS_NO_STDIO` 把它的文件 API
整个排除掉 —— 那套 API 收窄字符路径，在中文文件名上必然失败。
细节见 [third_party/stb/README.md](../third_party/stb/README.md)。

### 5.3 容器嗅探

`probe_container` 只看前 64 字节的魔数，**不看扩展名**。

支持识别：`RIFF/WAVE`、`FORM/AIFF`、`OggS`、`fLaC`、`ID3` 标签、
ASF/WMA GUID、偏移 4 的 `ftyp`、以及裸 MPEG 帧同步。

理由是实测过的一类真实故障：一个名叫 `.mp3` 的 ogg 交给 MF 只会得到
`MF_E_BYTESOURCE_UNKNOWN_TYPE`，然后被误判成"文件坏了"。
嗅探让这种文件被正确交给 stb_vorbis。

---

## 6. 已经落地的接口

```cpp
// core/failure.h —— 全项目共用的失败契约
struct Failure { std::string code; std::string context; };
// Failure::set 不覆盖已有原因：最内层的原因比外层包装更有价值

// core/path_utf8.h —— 唯一的 UTF-8 边界
std::string path_to_utf8(const std::filesystem::path&);
bool path_from_utf8(std::string_view, std::filesystem::path&, Failure&);
std::string wide_to_utf8(std::wstring_view);
bool utf8_to_wide(std::string_view, std::wstring&, Failure&);

// core/file_bytes.h —— 统一走宽字符 Win32 API，不用 ifstream(path)
bool read_file_bytes(const std::filesystem::path&, std::vector<std::uint8_t>&, Failure&, uint64_t max);
bool read_file_head(const std::filesystem::path&, std::size_t, std::vector<std::uint8_t>&, Failure&);

// core/logger.h —— JSONL，由构造函数注入，不是全局单例
class Logger { bool initialize_in_directory(const std::filesystem::path&, Failure&); ... };

// audio/container_probe.h
ContainerProbe probe_container(const std::vector<std::uint8_t>& head) noexcept;

// audio/decoder.h
bool probe_audio_file(const std::filesystem::path&, AudioInfo&, Failure&);
bool decode_audio_file(const std::filesystem::path&, DecodedAudio&, Failure&);
bool container_is_decodable(AudioContainer) noexcept;
```

---

## 6.5 窗口方案

阶段 2 已落地，实现在 `src/platform/`。

| 项 | 做法 | 为什么 |
| --- | --- | --- |
| 无系统标题栏 | `WS_POPUP` | 标题栏完全自绘 |
| 透明可见 | `WS_EX_NOREDIRECTIONBITMAP` | 不设这个，系统重定向位图会盖住 DirectComposition 的内容 |
| 逐像素透明 | DirectComposition + 预乘 alpha 表面，绘制前 `Clear(透明)` | 圆角、阴影、半透明全部由 alpha 表达 |
| 标题栏拖动 | `WM_NCHITTEST` 返回 `HTCAPTION` | 拖动、双击最大化、窗口吸附全由系统提供 |
| 坐标约定 | 回调只收**逻辑坐标**；窗口按 DPI 设缩放；render target DPI 固定 96 | 回调里永远不考虑物理像素，也不会两套缩放叠加 |
| 一个设备 | D3D11 → D2D 与 DirectComposition 共用 | 省一份显存与一次跨设备拷贝 |

**"渲染 DPI 必须是 96"是一条运行时不变量，不是假设**：不是 96 就说明 D2D 自己参与了缩放，
我们自己的乘法缩放会与它叠加、布局整体偏掉。代码里主动检查并在违约时明确失败
（`verify_render_dpi_contract`），而不是让它默默糊掉。

**阶段 3.5 补上了悬浮球。** 主面板与悬浮球共用 `TransparentWindow` 基类 ——
它们踩的是同一批坑（`WS_EX_NOREDIRECTIONBITMAP`、render target 的 DPI 钉在 96、
预乘 alpha、不能有背景刷、DComp 先于 D2D 释放），这些约定只该有一份。
悬浮球自己那份差别是：**圆形命中测试**（圆外返回 `HTTRANSPARENT`，
否则桌面上会多出一块看不见的方块吃掉点击）、拖动与点击的区分、不进任务栏、不抢焦点。

提取基类时发现**阶段 2 文档写的坐标契约在代码里没有落实**：
`resize_composition_surface` 从来没有 `SetTransform`，96 DPI 下看不出问题，
150% 下内容只会占左上角一块。已补上（`scale = dpi_ / kBaseDpi`），
并验证了 96 DPI 下帧图**逐字节不变**。

实测证据见 [阶段 2 验收记录](validation/2026-09-16-phase2-window-spike.md)：
四角 alpha 为 0、内侧为 255；命中测试自检 5/5；DPI 测试 30/30。

## 6.6 core 收尾

阶段 3 已落地。三条纪律决定了实现的形状：

| 纪律 | 做法 |
| --- | --- |
| **文件坏了不清空** | 内容不可用时把原文件重命名成 `.corrupt-<时间戳>` 并给出原因，从默认值开始 |
| **保存是原子的** | 写同目录临时文件 → `FlushFileBuffers` → `MoveFileExW(REPLACE_EXISTING \| WRITE_THROUGH)` |
| **崩溃留现场** | `MiniDumpWriteDump(MiniDumpNormal)`；写不出来至少留一个说明原因的失败标记 |

设置格式用行式 `key=value` 而不是 JSON，末尾一行 `complete=1` 用来检测"被截断但恰好断在
行边界"。越界取值夹到合法区间并记一条 note —— **静默纠正也是静默回退**。
比本程序更新的 schema 同样隔离（那是新版本写的文件，覆盖它会丢设置）。

单实例守卫是命名互斥体 **+ 一块共享内存携带已有 PID**：只用互斥体只能知道"已经有一个在跑"，
拿不到 PID 就没法把已有窗口叫到前面来。

实测证据见 [阶段 3 验收记录](validation/2026-09-16-phase3-core-closeout.md)：
`settings_tests` 88/88（含五种损坏形态的隔离）、`single_instance_tests` 18/18（跨进程）、
`crash_reporter_tests` 14/14（真崩一次并校验转储文件头）。

## 6.7 音频引擎

阶段 4 已落地，实现在 `src/audio/audio_engine.*` 与 `voice_timeline.*`。

**与 Godot 版的对应：**

| Godot 版 | 这里 |
| --- | --- |
| 手搓 `bgm_players` / `sfx_players` | `IXAudio2SourceVoice` 声部池，上限是编译期常量 |
| `AudioServer` 总线 | 两条 `IXAudio2SubmixVoice`（BGM / SFX） |
| `finished` 信号里重播并补淡入 | `XAUDIO2_LOOP_INFINITE` |
| 每帧 `delta` 累加做淡入淡出 | **绝对时间戳 + `VoiceTimeline`** |

**`VoiceTimeline` 存在的唯一理由是修掉那个具体问题**：原版"靠每帧 delta 累加，
`tree.paused` 时卡在半途音量上"。根因不是那个 pause，而是**用增量描述一个本该由时间决定的东西**。
现在给一个绝对时间戳就直接算出那一刻的增益，掉帧与挂起都不再有影响。

两条刻意的设计：声部数量**不做动态扩容**（跑团现场不该出现"第 11 个音效挤掉前面某个"）；
PCM 缓冲由槽位持有；2026-09-19 起在 `DestroyVoice` 确认混音器不再引用后释放。
“Stop/Flush 后保留一个 UI 周期”不构成可靠的内存所有权保证。

一个平台事实值得记下：**本机主声部是 48000 Hz，而曲库是 44100 Hz**，
重采样由 XAudio2 负责，上层不用管。

实测证据见 [阶段 4 验收记录](validation/2026-09-16-phase4-audio-engine.md)：
`voice_timeline_tests` 49/49、`audio_engine_tests` 77/77（含真实中文路径 ogg 的解码→播放端到端）。

## 6.8 曲库层

阶段 5 已落地，实现在 `src/library/`。URL 导入两条路线都接上了：
已知站点走 yt-dlp 子进程，直链走 WinHTTP 流式下载。

**分层：**

```
musicboard_core                 路径 UTF-8 边界、错误契约、日志、设置、单实例、崩溃转储
  ├── musicboard_audio          容器嗅探 + 解码契约 + 两个后端 + XAudio2 引擎
  │     └── musicboard_stb_vorbis
  ├── musicboard_library        扫描（只 probe）、文件操作与名字校验、导入事务、
  │                            URL 判定、子进程运行器、暂存区、导入作业
  └── musicboard_platform       窗口合成 + per-monitor DPI
```

**与 Godot 版的对应：**

| Godot 版 | 这里 |
| --- | --- |
| `audio_loader.gd` 扫描时把每个文件读进内存 | `scan_library` 只调 `probe_audio_file`，实测 491 文件 / 3.7 GB 的工作集增量约 0 MB |
| `create_category_folder("a/b")` 真的建出嵌套目录，加载器只扫一层 | 写入端在校验阶段就拒掉（`validate_category_name`），扫描端对已存在的嵌套目录忽略 |
| 重命名失败只写控制台，界面表现为"点了没反应" | 名字校验是纯函数，非法字符、22 个保留设备名、UTF-8 合法性、结尾空白或点全部在动盘之前判掉 |
| 删除直接从磁盘抹掉 | `DeleteMode::recycle_bin` 为默认，永久删除必须显式要求，并把实际用的方式回报给界面 |
| 下载直接写进曲库目录，再用 `_find_newest_file()` 猜哪个是刚下的 | 每任务一个空暂存目录 + `import_staged_file` 事务；**列目录，不猜**（0 个或 2 个都明确失败） |
| `cancel_import()` 只把标志位置回 false，下载照跑到完 | `ProcessRunner` 用 Job Object + `TerminateJobObject` 真杀进程树（yt-dlp 起的 ffmpeg 一起） |
| 进度只在开始和结束各发一次，百分比是下完之后才解析的 | 管道增量读，`ImportJobStatus.fraction` 在下载过程中真的在变 |
| 直链下载要求用户先装 yt-dlp | 直链走 WinHTTP 流式下载，只有 yt-dlp 才认识的站点才要求它 |
| 直链没有总长度时编一个百分比 | `has_total` 为假就是假：那时 `fraction` 恒为 0，界面显示已下载字节数 |
| yt-dlp 的完整输出只 `print_rich` 到控制台 | `extract_yt_dlp_error` + `suggest_yt_dlp_fix`：一句原因 + 一句可执行的建议，都上界面 |
| 扫描结果顺序随文件系统返回顺序变 | 分类与曲目都按 UTF-8 字节序排序，两次扫描结果完全相同 |

**唯一入口 `track_path_for`。** 这是本阶段最贵的一课：
`std::filesystem::path` 从窄字符串构造走**活动代码页**（本机 936/GBK），
`section_base / std::string("战斗")` 得到的不是"战斗"这个目录 ——
而 `create_category` 会返回成功、目录也确实建出来了，只是扫描器（走宽字符）找不到它。
用户看到的是"我建了分类，可它不出现"。现在所有"把 UTF-8 名字接成路径"的地方
都必须经过 `track_path_for`，头文件里写明了**绝不要写** `path / std::string`。

**两条刻意的退让**（都是为了让用户不改目录结构）：根下没有 `BGM` 子目录时把根当 BGM
目录（并排除 `SFX`）；栏目根下散着的音频文件收进一个额外的"默认"分类 ——
只扫一层的话它们会彻底消失，实测真实曲库就有 1 个这样的文件。

**URL 导入的关键设计：子进程是可控的。** `ProcessRunner` 非阻塞泵、
Job Object 连坐、增量读管道、卡死判定；`ImportJob` 把"读进度 → 列暂存目录 →
事务入库"串成状态机。测试用 `tests/fake_child.cpp` 当子进程，
并且让它以**真实 yt-dlp 参数**被调用（从 `-o` 模板解析出该写哪里）——
所以参数拼错时用例会直接红，而生产代码里不需要留任何测试入口。

实测证据见 [阶段 5 验收记录](validation/2026-09-16-phase5-library.md) 与
[URL 导入验收记录](validation/2026-09-16-phase5b-url-import.md)：
`library_ops_tests` 111/111、`import_transaction_tests` 52/52、
`library_scanner_tests` 68/68（对着真实 491 文件曲库）、
`url_classify_tests` 72/72、`http_download_tests` 57/57、
`process_runner_tests` 53/53、`import_job_tests` 132/132。
那两份记录里同时列了产品缺陷与测试缺陷 —— 其中绝大部分是测试抓出来的。

## 7. 验收记录

| 日期 | 阶段 | 记录 |
| --- | --- | --- |
| 2026-09-16 | 阶段 0 + 1 | [构建骨架与解码 spike](validation/2026-09-16-phase0-1-skeleton-decode.md) |
| 2026-09-16 | 阶段 2 | [透明覆盖层与窗口 spike](validation/2026-09-16-phase2-window-spike.md) |
| 2026-09-16 | 阶段 3 | [core 收尾：设置存储、单实例、崩溃转储](validation/2026-09-16-phase3-core-closeout.md) |
| 2026-09-16 | 阶段 4 | [XAudio2 音频引擎](validation/2026-09-16-phase4-audio-engine.md) |
| 2026-09-16 | 阶段 5 | [曲库层：扫描、文件操作、导入事务](validation/2026-09-16-phase5-library.md) |
| 2026-09-16 | 阶段 5 | [URL 导入：真实进度、真取消、不猜文件名（yt-dlp 与直链）](validation/2026-09-16-phase5b-url-import.md) |
| 2026-09-16 | 阶段 3.5 | [悬浮球窗口：圆形命中测试与共用透明窗口基类](validation/2026-09-16-phase3.5-orb-window.md) |
| 2026-09-16 | 阶段 6a | [曲库数据源：波形包络与封面提取](validation/2026-09-16-phase6a-library-data.md) |
| 2026-09-16 | 阶段 6b（1） | [界面交互内核与封面位图缓存](validation/2026-09-16-phase6b-ui-core.md) |
| 2026-09-16 | 阶段 6b（2） | [面板布局与全局快捷键](validation/2026-09-16-phase6b2-panel-layout-hotkeys.md) |
| 2026-09-16 | 阶段 6b（3） | [面板绘制与真实素材预览](validation/2026-09-16-phase6b3-panel-view.md) |
| 2026-09-16 | 阶段 6b（4） | [正式程序接线](validation/2026-09-16-phase6b4-application.md) |
| 2026-09-16 | 阶段 7 | [拖文件导入、通知区域、改键、安装卸载、打包](validation/2026-09-16-phase7-install-package.md) |

---

## 8. 后续阶段（尚未开始）

按依赖顺序，每阶段一个可运行、可检查的产物：

| 阶段 | 内容 | 为什么这个顺序 |
| --- | --- | --- |
| 产品接线 | URL 导入 UI、分类/曲目管理、双声部交叉淡化 | 后端通过不代表入口接齐 |
| 产品验收 | 真人听感/输入、音频设备变化、混合 DPI 与显示器热拔插 | 不能由内部 PASS 顶替 |

## 8.5 安装、卸载与打包

**校验是完整性边界。** `verify_install_package` 拒绝：绝对路径与 `..`（路径逃逸）、
哈希不符、**清单之外的文件**（否则能往包里塞一个会被加载的 dll）、
包内的重解析点、产品名或架构不符。清单自己不在清单里（列了就对不上），
所以它是包的信任根 —— 包外只有发布时公布的 zip SHA256，将来是代码签名。

**安装带日志分阶段**（verified → staged → previous_moved → candidate_published
→ committed）。先抄完整个包到候选目录再动 `current\`，所以"换上去"只是两次改名，
中间没有半成品窗口；任何一步断电都能按日志回滚或续做。

**只写 HKCU。** 装在 `%LOCALAPPDATA%\Programs\MusicBoard`、不需要管理员，
也就不该要求提权才能卸载。`UninstallString` 指向安装目录里那份 setup ——
卸载器与被装的版本天然一致。卸载**只删安装根**：曲库、设置、日志在别处，一律不动。

打包脚本 `tools/package_release.ps1` 产出包目录与 zip，并**对着真打出来的包**
跑一遍 `tools/smoke_packaged_setup.ps1`（21 项：装 → 查系统状态 → 篡改包必须被拒
→ 有未列文件的包必须被拒 → 卸 → 安装目录之外的用户数据没被动）。
那种"单元测试全绿、只有真装一遍才暴露"的问题（自我删除误删、注册表中文乱码）
就是这一步抓出来的。

## 9. 已经拍板的产品决定

这几条曾经是开放问题，现在有结论了，后续实现都按它们来。

| 决定 | 结论 | 影响 |
| --- | --- | --- |
| 音效台数量 | **固定 4 个**（2×2 网格） | 右列高度分配、`SfxSlotSettings` 的固定长度、无需动态增删台的界面 |
| 全局快捷键 | **要做**，用 `RegisterHotKey`，**不用**低级键盘钩子 | 需要冲突上报与开关；见 `src/platform/global_hotkeys.*` |
| 悬浮球 | 主入口（左键展开面板） | 阶段 3.5 已按此实现 |

关于快捷键为什么选 `RegisterHotKey` 而不是 `WH_KEYBOARD_LL`：

- **系统认可的做法。** 低级键盘钩子会让每个按键都经过我们的回调（有延迟），
  被安全软件当作键盘记录器告警，而且回调一旦慢了系统会**静默摘掉**它 ——
  那正是本项目最怕的失败方式。
- **失败是响亮的。** 组合键被别的程序占用时，`RegisterHotKey` 返回
  `ERROR_HOTKEY_ALREADY_REGISTERED`，我们可以明确告诉用户"这一条没注册上"。
- 代价是它会把按键从当前前台程序手里拿走。对一个用户明确设置的快捷键来说，
  这正是"快捷键"的定义，可以接受。
