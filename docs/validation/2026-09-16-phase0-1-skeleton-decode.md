# 阶段 0 + 1 · 构建骨架与音频解码 spike

日期：2026-09-16
状态：通过
素材：真实曲库 `D:\Desktop\音乐板\audio`（491 文件 / 3.7 GB / 42.7 小时）

这一篇记录的是**实际跑出来的数据**，不是设计意图。设计意图在
[../IMPLEMENTATION.zh-CN.md](../IMPLEMENTATION.zh-CN.md)。

---

## 1. 这一阶段要回答的问题

按方案的计划，阶段 1 是"唯一没有先例可抄的模块，先钉死它"。具体三个问题：

1. Media Foundation 能解哪些格式？不够的话缺哪一块、占真实曲库多少比例？
2. 中文 + 全角符号的文件名能不能全程走通？
3. probe 与 decode 分开，扫描真实曲库的代价是多少？

---

## 2. 实测过程与结论

### 2.1 环境

| 项 | 值 |
| --- | --- |
| 编译器 | MSVC 19.x（Visual Studio 2022 Community） |
| CMake | 4.4.2 |
| 系统代码页 | 936（简体中文） |

### 2.2 工具链上先踩到的两个坑

**(a) MSVC 默认按代码页 936 读源码**

```
warning C4819: 该文件包含不能在当前代码页(936)中表示的字符
error   C2065: "u8": 未声明的标识符
```

UTF-8 源码里的中文被打乱，连 `u8"..."` 字面量都解析不出来。
修法是编译期加 `/utf-8`。**不加这个开关，本仓库任何含中文的源文件都编译不过。**

**(b) `__cplusplus` 恒为 199711**

MSVC 默认不定义正确值，所有 C++ 版本判断失效。修法是 `/Zc:__cplusplus`。

### 2.3 编码差异的现场证据

同一条路径 `[低][休整] 和平 水边 轻松｜Fast Food.mp3`：

```
path.string()   -> 19 字节（GBK）
path.u8string() -> 26 字节（UTF-8）
```

这条差异已固化成测试用例（`core_tests` 里的"编码差异实证"），
因为它就是"为什么必须有单一 UTF-8 边界"的理由本身。

### 2.4 进程入口：`char** argv` 与 `getenv` 都是坑

第一次跑探针时全部文件返回 `0x80070003`（`ERROR_PATH_NOT_FOUND`），
但路径打印出来是正确的。原因是：

- MSVC 的 `char** argv` 按**活动代码页**（936）给出，不是 UTF-8。
- 程序把它按 UTF-8 解释 → 路径字节错乱 → 找不到文件。
- 而终端也是 GBK，所以打印出来"看着正常" —— 两处错得刚好对上。

修法是入口改用 `wmain` 拿宽字符 argv。**读环境变量同理**：`getenv` 同样走活动
代码页，必须用 `GetEnvironmentVariableW`。两条都写进了 `AGENTS.md` 的边界。

### 2.5 Media Foundation 的覆盖能力

```
[低][休整] 和平 水边 轻松｜Fast Food.mp3
  OK    [fromURL] native=MP3 -> PCM 44100Hz/2ch/16bit dur=302.03s firstFrame=yes
小鼠群.wav
  OK    [fromURL] native=PCM -> PCM 44100Hz/2ch/16bit dur=27.43s firstFrame=yes
[低][宁静] 平和 温暖.ogg
  FAIL  fromByteStream hr=0xC00D36C4        # MF_E_BYTESOURCE_UNKNOWN_TYPE
```

抽样 24 个文件：18 通过 / 6 失败。
**单独抽 8 个 ogg：0 通过 / 8 失败。**

另外发现 MF 的 `MFCreateSourceReaderFromURL` 不认正斜杠路径
（给 `D:/x/y.mp3` 返回 `ERROR_PATH_NOT_FOUND`），必须换反斜杠；
退化路径 `MFCreateFile` + `MFCreateSourceReaderFromByteStream` 可用。

**结论：必须为 ogg 引入第三方解码器。** ogg 占真实曲库 137/491（28%）。

### 2.6 选定 stb_vorbis 并锁哈希

上游 `nothings/stb`，v1.22，公共领域。

```
SHA256 = 4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636
```

编译时定义 `STB_VORBIS_NO_STDIO`，**排除它的文件 API** —— 那套走
`fopen` + `char*` 路径，在中文文件名上必然失败。本项目只走
`stb_vorbis_open_memory`，让这个坑在结构上不可能发生。

### 2.7 一个值得记下来的链接错误

第一次链接报：

```
musicboard_stb_vorbis.lib(stb_vorbis.obj) : error LNK2019:
    无法解析的外部符号 __imp_realloc，函数 stb_vorbis_decode_memory 中引用了该符号
```

原因是 stb_vorbis 那个目标漏了静态 CRT 设置，与其余 `/MT` 的目标混用了 `/MD`。
`__imp_` 前缀是 DLL 导入符号的标志，报错位置离真正原因（CRT 不一致）很远。

修法不是"给那个目标也补一条"，而是**把 CRT 改成全项目统一设置**
（`CMAKE_MSVC_RUNTIME_LIBRARY`，在任何 `add_library` 之前），
并把逐目标的那条删掉 —— 逐目标设置总有漏的一天。

### 2.8 `/WX` 抓到的一处

```cpp
read_duration(reader.Get(), audio.duration_seconds);   // 想表达"时长不是必需"
```

编译失败：`warning C4834: 放弃具有 [[nodiscard]] 属性的函数的返回值`。

这正是这条纪律想要的效果 —— 它逼着人把"我是故意忽略的"写出来：

```cpp
static_cast<void>(read_duration(...));   // 显式丢弃，表明有意为之
```

---

## 3. 验收数据

### 3.1 测试

```
core_tests              33/33 通过
container_probe_tests   20/20 通过
decoder_tests           21/21 通过        （对着真实曲库）
------------------------------------------------
合计                    74/74 通过
```

`decoder_tests` 里几条关键断言：

```
PASS ogg 能解码
PASS ogg 由 stb_vorbis 后端处理
PASS mp3 由 Media Foundation 后端处理
PASS 找到含非 ASCII 的路径: ...\[低][休整] 和平 水边 轻松｜Fast Food.mp3
PASS 非 ASCII 路径可以探测
PASS 非 ASCII 路径可以解码
PASS probe 与 decode 在所有样例上格式一致、时长差在 0.5 秒内
PASS 非音频内容给出 unsupported_format 而不是 decode_failed
```

最后一条是有意区分的：内容不是音频时要说"格式不支持"，
而不是含糊地报"解码失败"。

### 3.2 全曲库扫描（阶段 1 的核心证据）

```
$ scan_library.exe "D:/Desktop/音乐板/audio"
扫描曲库: D:/Desktop/音乐板/audio
发现 491 个音频文件

---- 结果 ----
列目录耗时      : 3 ms
probe 耗时      : 3600 ms  (平均 7.33 ms/文件)
总时长          : 42.7 小时
内存占用增量    : 6 MB
按容器统计      :
    mp3      350
    ogg      137
    wav      4
成功 491 / 491
```

**内存增量 6 MB vs 曲库 3.7 GB。**

Godot 版在扫描阶段就对每个文件 `get_buffer(get_length())`，
同样这份曲库需要 GB 级常驻内存。相差约 600 倍。

这不是优化出来的，是接口设计出来的：把"看过"和"取出"拆成两个操作。
所以这条写进了持续边界。

---

## 4. 这一阶段改变了哪些设计判断

| 原判断 | 实测后的调整 |
| --- | --- |
| 用 Media Foundation 覆盖 mp3/wav/aac/flac，ogg 交 stb_vorbis | 确认。但**必须**先嗅探容器再分派，不能靠扩展名 |
| 按需解码 | 确认并量化：6 MB vs 3.7 GB，写进持续边界 |
| 单一 UTF-8 边界 | 确认，并**扩大到进程入口**（`wmain`）与环境变量（`GetEnvironmentVariableW`） |
| 依赖锁到 SHA256 | 确认。stb_vorbis 哈希已记录 |
| —— | 新增：CRT 必须全项目统一设置，不能逐目标 |

---

## 5. 已知未覆盖

诚实列出这一阶段**没有**验证的东西：

- **只有这台机器上的 MF 部件。** 不同 Windows 版本/语言版本的 MF 解码器集合
  可能不同（例如某些系统带 Ogg 的第三方解码器）。因此嗅探 + 双后端的结构不能省，
  即使某台机器上 MF 恰好能解 ogg。
- **mp4/m4a/wma/aiff/flac 只验了嗅探，没有验解码** —— 真实曲库里没有这些格式的样本。
  对应的后端代码路径（走 MF）与 mp3 相同，但没有实测数据。
- **48kHz / 单声道 / 24 位 等非 44100/2/16 的样本**没有覆盖：真实曲库里全部是
  44100Hz 立体声。后端里已经写了"MF 给出非 16 位就明确失败"的保护，但没有样本能触发它。
- **未做性能剖析**。7.33 ms/文件是端到端观测值，没有分解到"打开容器"与"取时长"各占多少。
- **并发扫描未验证**。当前是单线程遍历。

---

## 6. 复现方式

```bash
cmake --preset windows-x64
cmake --build --preset release

export MUSICBOARD_TEST_LIBRARY="D:/Desktop/音乐板/audio"
ctest --preset release --output-on-failure

./build/windows-x64/Release/scan_library.exe "D:/Desktop/音乐板/audio"
```

`decoder_tests` 在 `MUSICBOARD_TEST_LIBRARY` 未设置时会 `SKIP` 相关用例而不是失败 ——
测试的成败不该取决于某台机器上有没有素材。
