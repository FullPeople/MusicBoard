# 阶段 4 · XAudio2 音频引擎

日期：2026-09-16
状态：通过
目标：把"能不能播"做出来 —— 声部池、两条独立总线、绝对时间的淡入淡出、无限循环

---

## 1. 前置探针：这台机器能不能出声

先问了再写。XAudio2 在没有输出设备时（虚拟机、远程桌面、无声卡）会直接失败，
不问清楚就写引擎，测试会变成"在别人机器上过、在我这里崩"。

```
$ audio_probe.exe
CoInitializeEx:                 0x00000000
XAudio2Create:                  0x00000000
CreateMasteringVoice:           0x00000000
主声部: 输入声道=2 采样率=48000
CreateSubmixVoice:              0x00000000     ← BGM/SFX 两条总线靠它
CreateSourceVoice(默认格式):     0x88960001     ← XAUDIO2_E_INVALID_CALL，符合预期
```

两条有用的结论：

1. **有可用设备**，引擎可以真跑，测试不是空转。
2. **主声部是 48000 Hz，而曲库是 44100 Hz。** 这个差异由 XAudio2 自己重采样处理，
   上层不用管 —— 但值得写下来，免得以后有人看到两个数字就动手"修"。

探针过程中还踩到一个自己造的段错误：用了 `ComPtr<IXAudio2>` 又手动 `engine->Release()`，
双重释放。而且因为 stdout 有缓冲，崩溃时输出全丢，看起来像"什么都没跑到就死了"。
修法是去掉手动 Release，并把 stdout 设成无缓冲。

---

## 2. 两块实现

### 2.1 `voice_timeline` —— 绝对时间的淡入淡出

**这一块存在的唯一理由是修掉 Godot 版的那个具体问题**：

> 淡入淡出靠每帧 `delta` 累加，打开文件夹对话框（`tree.paused`）时会卡在半途音量上。

根因不是那个 pause，而是**用增量描述一个本该由时间决定的东西** ——
只要有一帧没跑到，音量就永远回不到正确值。

这里反过来：调用方给一个绝对时间戳，`gain_at()` 直接算出那一刻该是多少。
于是掉帧、卡顿、被挂起都不再有影响，暂停也不需要任何特殊处理（时间不推进，增益自然不变）。

### 2.2 `audio_engine` —— XAudio2

与 Godot 版的对应：

| Godot 版 | 这里 |
| --- | --- |
| 手搓 `bgm_players` / `sfx_players` | `IXAudio2SourceVoice` 声部池，上限是**编译期常量** |
| `AudioServer` 总线 | 两条 `IXAudio2SubmixVoice`（BGM / SFX） |
| `finished` 信号里重播并补淡入 | `XAUDIO2_LOOP_INFINITE`，交给音频线程 |
| 每帧 `delta` 累加 | 绝对时间戳 + `voice_timeline` |

两条刻意的设计：

- **声部数量不做动态扩容。** 跑团现场不该出现"第 11 个音效把前面某个挤掉"这种不可预期的事。
  满了 `find_free_slot` 明确报告没有空位，由上层决定怎么办。
- **PCM 缓冲的生命周期由槽位持有。** XAudio2 直接读那块内存，所以换曲前必须先
  `Stop` + `FlushSourceBuffers`；旧缓冲再留一个 update 周期才真正释放
  （`retired_pcm`），给音频线程留收尾时间。

---

## 3. 验收数据

```
$ ctest --preset release
1/9 core_tests                Passed
2/9 settings_tests            Passed
3/9 single_instance_tests     Passed
4/9 crash_reporter_tests      Passed
5/9 dpi_tests                 Passed
6/9 container_probe_tests     Passed
7/9 voice_timeline_tests      Passed
8/9 audio_engine_tests        Passed
9/9 decoder_tests             Passed
100% tests passed out of 9
```

分项：

| 测试 | 断言数 | 覆盖 |
| --- | --- | --- |
| `voice_timeline_tests` | 49/49 | 淡入淡出形状、单调性、边界、**查询方式无关性** |
| `audio_engine_tests` | 77/77 | 真 XAudio2：播放/暂停/跳转/循环/总线与槽位独立/真实文件端到端 |

### 3.1 关键用例：长时间卡顿后增益仍然正确

这是修掉"每帧累加"的判据：

```
PASS 带 1 秒淡入播放
PASS 卡顿前增益还很低            [0.2 秒时增益 0.19]
PASS 卡顿 20 秒后第一次 update 就把增益落到正确值（不是停在半途）
```

### 3.2 关键用例：暂停真的冻结了位置

不是把音量调 0 假装暂停 —— 那样音频线程还在推进播放位置，界面上会看到"暂停了进度还在走"。

```
PASS 暂停期间播放位置冻结（不是把音量调 0 假装暂停）
PASS 暂停前确实播了一点（否则这条测不出东西）
PASS 恢复后位置继续推进
```

### 3.3 真实文件端到端

```
PASS 解码真实 ogg（中文路径）
PASS 把解出来的 PCM 交给引擎播放
PASS 引擎报告的时长与解码器一致
```

从曲库里挑一个 **ogg**（走 stb_vorbis 后端）、**中文 + 全角符号路径**，
解成 PCM 再交给引擎播 —— 整条链路一次走通。

---

## 4. 过程中发现并修掉的问题

### 4.1 `SamplesPlayed` 不会被 `FlushSourceBuffers` 重置（真 bug）

`seek` 的实现是先 `Stop` + `FlushSourceBuffers`，再从目标样本重新提交缓冲。
我原以为声部的 `SamplesPlayed` 会从 0 重新计数，于是在报告位置时"加一个固定偏移"。

实测：跳到 12.5 秒后报告 **13.58 秒**，越报越大。
原因是 `SamplesPlayed` 是声部一生的累计值，Flush 不动它。

修法：记录 seek 那一刻的 `SamplesPlayed` 作基线，位置算成
「seek 目标 + 相对基线又播了多少」。

### 4.2 XAudio2 的 voice 接口不是 COM 引用计数的

用 `ComPtr<IXAudio2SourceVoice>` 编译不过 —— `wrl/client.h` 找不到 `Release()` 成员。
XAudio2 的 voice 必须用 `DestroyVoice()` 销毁，没有 `AddRef` / `Release`。
改用裸指针手工管理，与主声部/总线声部保持一致。

### 4.3 `windows.h` 的 `min`/`max` 宏破坏了 `std::max`

```
error C2589: "(" : "::" 右边的非法标记
```

`windows.h` 默认把 `min`/`max` 定义成宏，`std::max(0.0, x)` 被展开成语法错误。
这类问题该在编译选项里一次性解决，不是逐处加括号 —— 已在
`cmake/CompilerOptions.cmake` 里加了 `NOMINMAX`（和 `UNICODE`/`_UNICODE` 并列）。

### 4.4 两个自找的测试缺陷

- **`pump()` 每次重新锚定真实时间**，于是两次"推进"不是接续的，累计虚拟时间比设想的小，
  误报"淡入没到满增益"。改成所有用例共用一条连续的虚拟时钟。
- **`getenv` 读曲库路径** —— 这和 `char** argv` 是同一个坑，`decoder_tests` 里已经修过一次，
  我又在新测试里犯了一遍。已改用 `GetEnvironmentVariableW`。

### 4.5 `/WX` 抓到的

三处裸调 `[[nodiscard]]` 的 `stop()`。这些是清理路径，不关心结果，但要显式写出
"有意忽略" —— 这条纪律的价值就在这。

---

## 5. 已知未覆盖

- **没有验证"听到的声音对不对"。** 这一层验的全是状态与数值：声部活着没有、增益是多少、
  位置走没走。**音色、有没有爆音、淡入淡出听感如何，只能靠人耳** —— 本阶段没有做真人试听。
- **没有捕获实际输出做波形比对。** 要抓 XAudio2 的输出需要自定义效果器或 APO，
  工作量大，本阶段没做。所以"增益数值算对了"与"输出真的按那个增益混了"之间
  还隔着一层没有实测的假设。
- **两条总线的音量只验了引擎侧记录的数值**，没有验证混合后的实际电平。
- **`SetOutputVoices` 的调用方式可能有多余**：`play()` 里先解除再设置，是为了避免重复连接，
  但没有实测"不解除会怎样"。
- **seek 只测了非循环场景**。循环播放中 seek 的行为（`LoopCount` 与偏移的配合）没有覆盖。
- **多线程安全未验证。** `update()` 假定在单线程调用；`play/stop` 若从别的线程调用，
  与 `update` 之间没有锁。当前设计是"全部在 UI 线程"，但没有实测或断言守住这一点。
- **长时播放的稳定性未验证**（比如连续循环几小时会不会有累积误差或资源泄漏）。

---

## 6. 复现方式

```bash
cmake --preset windows-x64
cmake --build --preset release

# 先确认这台机器有音频输出设备
./build/windows-x64/Release/audio_probe.exe

export MUSICBOARD_TEST_LIBRARY="D:/Desktop/音乐板/audio"
ctest --preset release --output-on-failure
```

`audio_engine_tests` 在没有输出设备时会把全部引擎用例标成 `SKIP` 而不是 `FAIL` ——
测试的成败不该取决于跑在哪台机器上。
