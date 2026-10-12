# 阶段 5 · 曲库层（扫描 / 文件操作 / 导入事务）

日期：2026-09-16
范围：`src/library/`（`library_scanner`、`library_ops`、`import_transaction`）、
`musicboard_library` 静态库、三个测试可执行。
对应正本：`docs/IMPLEMENTATION.zh-CN.md` 阶段 5。
URL 导入（真实进度与取消、进程管理）不在本次范围，见文末"未完成"。

## 一句话结论

曲库层已可用并通过全部测试；对着真实的 491 文件 / 3.7 GB 曲库跑，
**4 个测试缺陷和 3 个产品缺陷是测试抓出来的，不是读代码看出来的** ——
其中两个会让用户直接看到"东西不见了"。这 7 条都记在下面。

## 测试结果

```
core_tests               33/33
settings_tests           88/88
single_instance_tests    18/18
crash_reporter_tests     14/14
dpi_tests                30/30
container_probe_tests    20/20
voice_timeline_tests     49/49
audio_engine_tests       77/77
decoder_tests            21/21
library_ops_tests       111/111
import_transaction_tests 52/52   （1 条 SKIP，见"覆盖不到的分支"）
library_scanner_tests    68/68
```

`ctest --preset debug`：12/12 通过，33.66 秒。
Debug 配置、`/W4 /WX`，零警告。

## 真实曲库实测

`MUSICBOARD_TEST_LIBRARY=D:\Desktop\音乐板\audio`，491 个文件 / 3.7 GB。

```
真实曲库: 491 个候选, 491 成功, 0 失败, 20 个 BGM 分类, 2 个 SFX 分类,
          耗时 3.15 秒, 工作集增量 2.3 MB
```

证据：`evidence/library-scan-real.txt`（完整 PASS 列表）。

三条要点：

1. **491 个候选全部成功**。上一次统计是 490 —— 差的那一个就是下面第 3 条产品缺陷。
2. **工作集增量 2.3 MB**（上一轮 0.9 MB，波动来自 Media Foundation 的惰性分配）。
   这条数字是"扫描只 probe、不解码"这个设计存在的全部理由：
   真实曲库 3.7 GB，一旦在扫描阶段取 PCM，常驻内存立刻变成 GB 级。
3. 491 个文件、3.7 GB、3.15 秒。工具链是 Debug 构建，Release 会更快。

## 测试抓出来的缺陷

### 产品缺陷（会影响用户）

**1. `path / std::string` 把 UTF-8 名字按 936 代码页打乱 —— 分类建了却不显示**

`std::filesystem::path` 从窄字符串构造时走**活动代码页**。本机是 936/GBK，
于是 `section_base / std::string("战斗")` 得到的目录名不是"战斗"：

```
"战斗"（UTF-8，6 字节 E6 88 98 E6 96 97）→ 按 GBK 解释 → 磁盘上多出一个乱码目录
```

表现最恶劣的地方是它**不报错**：`create_category` 返回成功、目录确实建出来了，
但扫描器走宽字符 API，找不到那个名字。用户看到的是"我建了分类，可它不出现"。

修复：`library_ops.h` 新增唯一入口 `track_path_for(directory, stem, extension)`，
所有"把 UTF-8 名字接成路径"的地方都经过它（`create_category`、`rename_category`、
`delete_empty_category`、`rename_track`、`move_track`、`unique_path_in`、
`import_transaction`），并在头文件里写明**绝不要写** `path / std::string`。
同时 `common_name_check` 增加"必须是合法 UTF-8"这一条 ——
不先挡它，后面构造路径时才会失败，而那个失败更难解释。

> 这条是本阶段最重要的一处。它也是"路径只能有一个边界"这条纪律第一次
> 在测试里被真正验证，而不是靠读代码断言。

**2. 重名避让的判断少拼一个扩展名，导致对任何名字都报"发生过重名"**

`renamed_to_avoid_collision` 的比较写成 `target != dir / (stem + extension)`，
而 `target` 来自 `unique_path_in`（走 UTF-8 边界）。两边不同源，比较恒为真。
修法不是补一个字符串，而是让两边由同一个函数算出来：
`unique_path_in` 的第一个候选和调用方"本来想要的名字"都用 `track_path_for`。

**3. `BGM/` 根下散着的音频文件被静默丢掉（真实曲库就有 1 个）**

只扫一层 + "有子目录就不扫本层文件"这两条合起来，会让
`BGM/背景恐怖音2.mp3` **彻底消失**：磁盘上明明在，界面上永远看不到，也没有任何提示。
这正是本项目列为头号要避免的失败方式。

修复：栏目根下散落的音频文件收进一个额外的"默认"分类，
一个候选都没有时不会凭空多出这个分类（`keep_when_empty=false`）。
真实曲库的候选数因此从 490 变成 491。

顺带修了同一处的另一个问题：BGM 退让到曲库根目录时，
`<根>/SFX` 会被当成 BGM 的一个分类扫进来，而用户放在根下的音乐一首都不显示。
现在扫描栏目时排除 `SFX`（大小写不敏感，因为 Windows 文件名不区分大小写）。

**4. 全角空格既不算空白也不算有内容**

`is_space`/`has_visible` 原来是按字节判的。全角空格 U+3000 是 `E3 80 80`，
看字节完全看不出"这是空白"，于是"　　"（两个全角空格）能通过校验。
而 Windows 存盘时会把结尾的空格（含 U+3000）吃掉 ——
结果就是"文件名和界面显示不一致"。

修复：`common_name_check` 与 `sanitize_to_valid_name` 改为**逐码点**判断，
覆盖 U+3000、U+00A0、U+2000–U+200A、U+2028/2029、U+202F、U+205F。
这也让"结尾是空白或点"的拒绝规则对中文输入法真正生效。

### 测试自身的缺陷（4 条，值得单独记）

1. **`"a\x01b"` 里的 `\x01b` 被当成十六进制转义 0x1B** ——
   这是 C++ 的最长匹配规则。写 `std::string("a\x01") + "b"` 才是想测的东西。
2. **前缀陷阱测错了对象**：原来比较 `root` 与 `root/BGMX/战斗`，
   那本来就是包含关系，测不出"`BGMX` 不被 `BGM` 前缀误判"。
   改成比较真正的两个兄弟目录 `root/BGM` 与 `root/BGMX`。
3. **Debug CRT 断言弹模态框 + stdout 块缓冲 = 现场什么都没有**。
   第一次跑 `library_ops_tests` 只拿到一个退出码和一个对话框截图，
   一条 PASS/FAIL 都没留下。现在 `test_harness.h` 的 `Registry` 构造时
   就把 stdout 设成不缓冲，并把 `_CRT_ASSERT`/`_CRT_ERROR` 改写 stderr、
   不弹窗。在没人看的自动化里，模态框等于永久挂起，这一条是硬需求。
4. **`write_test_wav`/`write_text` 的返回值被丢弃**（`[[nodiscard]]`）→ C4834 被 `/WX` 拦下。
   全部改成显式 `static_cast<void>(...)`。

## 覆盖不到的分支（不假装测过）

`import_transaction.cpp` 里三段代码在自动化测试里到不了：

| 分支 | 为什么到不了 | 怎么覆盖 |
| --- | --- | --- |
| 跨盘复制 → 校验大小 → 删源 | 测试目录永远在同一个卷上，同盘 `rename` 就成功了 | 代码审阅；需要真实双卷环境 |
| 发布后重新探测失败 → 回滚删除 | 发布前后的 probe 读的是同一份字节，除非发布本身写坏数据，否则不可能一个过一个不过 | 代码审阅 + 断点观察 |
| 符号链接被拒 | 建符号链接需要开发者模式或管理员权限，本机没有（测试已试过 `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`） | 测试里 SKIP；有特权的机器上会自动跑 |

还有一条**刻意放弃**的覆盖：用目录 junction（不需要权限）测"重解析点不能把文件
偷运出暂存区"。放弃的理由是它需要在测试里手写 `FSCTL_SET_REPARSE_POINT`
与 `MOUNT_POINT_REPARSE_BUFFER`（约 45 行易错的 Win32 结构代码），
而它验证的 `path_is_inside` 已经被真实路径的逃逸用例覆盖。
写错这个 helper 会得到一个更糟的结果：一个不可靠的测试。

## 设计取舍（可能被质疑的两处）

**`delete_track` 默认走回收站，不是永久删除。**
设计稿里写的是"删除"。实现选了 `DeleteMode::recycle_bin`（`SHFileOperationW`
+ `FOF_ALLOWUNDO`）。用户的音乐库是私人财产，误删一下不该是不可逆的。
要永久删除必须显式传 `DeleteMode::permanent`，并且 `LibraryOpResult.delete_mode`
会把实际用的方式回报给界面。

**`delete_empty_category` 拒绝非空目录，不递归。**
递归删除是在替用户做他没想到的决定。非空时返回
`common.invalid_argument` 并说明"请先移走或删除其中的文件"。

**扫描只忽略、写入端才拒绝。**
分类名里出现 `/` 或 `..`，`create_category` 在校验阶段就拒掉（这是原版那个
"建出嵌套目录、文件永远不显示"的 bug 的正解）；扫描端遇到已经存在的嵌套目录
则是忽略。两边合起来才严密：一边不让它产生，一边不让它被误解。

## 本阶段新增的能力清单

- `musicboard_library` 静态库（依赖 `core` + `audio`，链 `shell32`）
- `scan_library`：只 probe、可取消、进度回调、逐条 warning、结果确定性排序
- `track_path_for` / `unique_path_in` / `path_is_inside`：路径安全的唯一入口
- `validate_file_stem` / `validate_category_name`：穷举可测的纯函数
  （非法字符、22 个保留设备名含带扩展名、控制字符、UTF-8 合法性、
  结尾空白或点、长度上限、`.` 与 `..`）
- `sanitize_to_valid_name`：按码点净化，截断不切进多字节序列
- `delete_track` / `rename_track` / `move_track` / `create_category` /
  `rename_category` / `delete_empty_category`
- `import_staged_file`：校验来源（含路径逃逸与符号链接）→ 校验内容 →
  原子发布 → 发布后复检并回滚；**永不覆盖**，重名自动加 ` (2)`

## 未完成

- **URL 导入的真实进度与取消**（进程管理：`CreateProcess` + 增量读管道 +
  `TerminateProcess`）。原版的问题是把下载直接写进曲库目录再用
  `_find_newest_file()` 猜哪个是刚下的（`url_importer.gd:449`）；
  暂存区 + `import_staged_file` 这一半已经就位，缺的是下载器本身。
- 两个产品决定待定（本阶段不阻塞，但会影响阶段 6 的排版）：
  SFX 台位固定 4 个还是扩到 6/8；是否做全局快捷键。
