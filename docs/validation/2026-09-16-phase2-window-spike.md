# 阶段 2 · 透明覆盖层与窗口 spike

日期：2026-09-16
状态：通过
目标：打掉 Godot 版最刺眼的问题 —— 透明窗口配了没生效、系统标题栏与自绘标题栏同时存在

这一篇记录**实际跑出来的数据**。设计意图在 [../IMPLEMENTATION.zh-CN.md](../IMPLEMENTATION.zh-CN.md)。

---

## 1. 这一阶段要回答的四个问题

1. 无边框窗口能不能做到**真正的逐像素透明**（圆角外透出桌面）？
2. **DirectWrite 排中文**对不对？长曲名能不能显示成省略号？
3. **per-monitor DPI**：布局跟着缩放、绘制不被二次缩放，这个不变量守得住吗？
4. 自绘标题栏的拖动能不能走系统（保留窗口吸附与双击最大化）？

---

## 2. 实现要点

| 项 | 做法 | 为什么 |
| --- | --- | --- |
| 无系统标题栏 | `WS_POPUP` | 标题栏完全自绘 |
| 透明可见 | `WS_EX_NOREDIRECTIONBITMAP` | 不设这个，系统重定向位图会盖住 DirectComposition 的内容 |
| 逐像素透明 | DirectComposition + 一个预乘 alpha 的 `IDCompositionSurface`，绘制前 `Clear(透明)` | 圆角、阴影、半透明全部由 alpha 表达 |
| 标题栏拖动 | `WM_NCHITTEST` 返回 `HTCAPTION` | 拖动、双击最大化、窗口吸附全部由系统提供，不用自己算鼠标偏移 |
| 坐标约定 | 绘制回调只收**逻辑坐标**；窗口按 DPI 设缩放；render target 的 DPI 固定 96 | 回调里永远不考虑物理像素，也不会出现两套缩放叠加 |
| 一个设备 | D3D11 → D2D 与 DirectComposition 共用 | 不各建一套，省一份显存与一次跨设备拷贝 |
| 文本抗锯齿 | 灰度而不是亚像素 | 亚像素假设背景不透明，叠在桌面上会出现彩边 |

---

## 3. 验收数据

### 3.1 透明：四角 alpha 实测

把同一份绘制回调渲染到 PNG（`--png`），再逐像素读回 alpha 通道。

```
尺寸 1256x788, 通道 4（RGBA，有 alpha）

=== 四角（圆角外应当完全透明）===
  左上(0,0)   RGBA = (0, 0, 0, 0)
  右上        RGBA = (0, 0, 0, 0)
  左下        RGBA = (0, 0, 0, 0)
  右下        RGBA = (0, 0, 0, 0)

=== 圆角内侧（应当是面板底色，不透明）===
  左上内侧(20,20)   RGBA = (20, 22, 27, 255)
  右上内侧          RGBA = (20, 22, 27, 255)

=== 中心区域 ===
  正中心          RGBA = (26, 29, 36, 255)
```

四角 alpha 为 0 而内侧为 255，说明圆角处是真的透气，不是"画了一块深色"。
内侧值 `(20,22,27)` 与 `(26,29,36)` 分别对应设计令牌里的 `--bg-window` 与 `--surface-1`。

**桌面上的人工确认**：把窗口跑起来后截桌面，能直接看到面板浮在其他窗口之上、
圆角处透出后面的内容、顶部没有系统标题栏（只有自绘的那一条）。

### 3.2 自绘标题栏的命中测试

不去模拟鼠标拖动（那需要窗口在最前台），而是**直接问窗口自己**：
`SendMessage(hwnd, WM_NCHITTEST, ...)` 看它返回什么。这测的正是那段逻辑本身。

```
$ spike.exe --selftest
窗口已创建: 1256 x 788 逻辑像素, 当前 DPI 96, 物理 1256 x 788
PASS 标题栏内 → HTCAPTION（拖动走系统）  (逻辑 60,20 -> 2)
PASS 标题栏右端 → HTCAPTION  (逻辑 1230,20 -> 2)
PASS 标题栏下方 → HTCLIENT（交给自绘控件）  (逻辑 200,100 -> 1)
PASS 面板中部 → HTCLIENT  (逻辑 600,400 -> 1)
PASS 标题栏下边界外 → HTCLIENT  (逻辑 60,43 -> 1)
---- spike 自检: 5/5 通过 ----
```

最后一条特意打在 43 逻辑像素处（标题栏高 42），确认边界精确、没有多一个像素的拖动带。

### 3.3 DPI

```
$ dpi_tests.exe
---- dpi_tests: 30/30 通过，0 失败 ----
```

覆盖：96/125/150/175/200% 下的面板尺寸、小尺寸取整、逻辑↔物理往返、
浮点缩放不做取整（它喂给 DirectWrite，取整会累积成行高误差），
以及**渲染 DPI 契约**（不是 96 就明确失败并说明原因）。

### 3.4 全量测试

```
$ ctest --preset release
1/4 core_tests                Passed
2/4 dpi_tests                 Passed
3/4 container_probe_tests      Passed
4/4 decoder_tests              Passed
100% tests passed out of 4
```

---

## 4. 渲染结果

`evidence/stage2-frame-96dpi.png` 是 spike 渲染出来的一整帧，与屏幕上看到的一致
（走同一份绘制回调）。在这一帧上确认了：

- 中文全部正常（标题栏、曲名、分类、标签、芯片、总线状态）
- 长曲名显示成省略号（卡片上的 `01 - Curse of Stra…`）
- 音波图是**连续包络**而不是一排矩形，已播/未播用遮罩切开
- 唱片、唱臂、竖向功能键、竖向音量条、总线状态点都按设计令牌画出来了
- 搜索框与悬停预览区在同一个盒子里（左输入框、右预览，竖线分隔）

---

## 5. 过程中踩到的坑

这一阶段有四个坑值得记下来，都不在原来的预料里。

### 5.1 `small` 是个宏

```cpp
ComPtr<IDWriteTextFormat> small;   // 编译器报 "ComPtr<...> 后面接 char 是非法的"
```

`rpcndr.h` 把 `small` 定义成 `char`（16 位 Windows 的遗留）。成员名被宏展开成 `char`，
而报错信息完全指不到真正的名字。改用 `caption`。

**这类错误的特征是"报错说的 token 你在源码里根本找不到"。**

### 5.2 `constexpr D2D1_COLOR_F` 编不过

`D2D1::ColorF` 的构造函数不是 `constexpr`，所以颜色常量全废，后面几十处使用连锁报错
（`error C2131: 表达式的计算结果不是常数` 加一堆看不懂的下游错误）。
改用 C++17 的 `inline const`：同样是编译期常量语义，但不受 `constexpr` 限制。

### 5.3 `CreateSolidColorBrush` 收引用不收指针

```cpp
target.CreateSolidColorBrush(color, nullptr, &brush);   // 失败
```

C++ 重载的第二个参数是 `const D2D1_BRUSH_PROPERTIES&`，传 `nullptr` 不行。要传一个对象。

### 5.4 `D2D1CreateFactory` 的模板重载不可见

这个 SDK 配置下只暴露收 `void**` 的原始重载，模板形式不在候选里。
用 4 参数原始形式加显式 `reinterpret_cast<void**>` 即可。

### 5.5 两个自找的

- 给 `--selftest` 也设了 `exit_after_png`，导致自检块之前就返回，输出里什么都没有。
- 一次 `WM_SIZE` 里忽略了 `[[nodiscard]]` 返回值，被 `/WX` 拦下（这是它该做的）。

---

## 6. 已知未覆盖

- **没有真的触发 `WM_DPICHANGED`。** 本机只有一块主屏（3440×1440，100%），
  没法把窗口拖到不同 DPI 的显示器上。DPI 的**算术**有 30 条测试覆盖，
  但"跨屏拖动时窗口尺寸与合成表面重建"这条路径只在代码里写了，没有实机跑过。
- **没有验证窗口在 125%/150% 缩放下的实际观感。** 逻辑坐标与渲染 DPI 的契约有测试，
  但"布局在 150% 下看起来对不对"只能靠人眼，本机没法切缩放。
- **圆形窗口 / 不规则点击区未做。** 现在窗口对系统来说是矩形，
  圆角只是视觉上的 —— 点在圆角外的透明区域仍算点在窗口内。
  要做真正的异形命中，得用 `SetWindowRgn` 或分层窗口的 alpha 命中测试。
- **多显示器热插拔未验证。**
- **性能未测。** 没有测帧率、绘制耗时、显存占用。
- **悬浮球那种小尺寸分层窗口（`UpdateLayeredWindow`）没有做。** 本阶段只做了主面板的
  DirectComposition 路线。设计上两者共用同一个 D3D11 设备，但那部分还没写。

---

## 7. 复现方式

```bash
cmake --preset windows-x64
cmake --build --preset release

# 命中测试自检
./build/windows-x64/Release/spike.exe --selftest

# 渲染一帧成 PNG（走与屏幕相同的绘制回调）
./build/windows-x64/Release/spike.exe --png evidence/stage2-frame-96dpi.png --exit

# 跑起来看透明与无边框（需要人眼）
./build/windows-x64/Release/spike.exe

# 全量测试
ctest --preset release
```
