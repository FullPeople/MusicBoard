# 阶段 3 · core 收尾：设置存储、单实例、崩溃转储

日期：2026-09-16
状态：通过
目标：把"数据安全"与"进程身份"这两件事做完 —— 它们都是纯逻辑，可以完全离线测

---

## 1. 这一阶段要回答的三个问题

1. **文件坏了会不会把用户的配置一起丢掉？**（Godot 版是只写控制台然后什么都不做）
2. **双击第二次 EXE 会不会起第二套覆盖层与音频？**
3. **崩了之后磁盘上有没有留下可诊断的现场？**

---

## 2. 设置存储

### 2.1 三条决定实现形状的纪律

| 纪律 | 具体做法 |
| --- | --- |
| **文件坏了不清空** | 内容不可用时把原文件重命名成 `.corrupt-<时间戳>` 并给出原因，然后从默认值开始 |
| **版本要能迁移** | 每个文件带 `schema` 号；旧版本按迁移链升级；**比本程序更新的版本同样隔离**（那是新版本写的，覆盖它会丢设置） |
| **保存是原子的** | 写同目录临时文件 → `FlushFileBuffers` → `MoveFileExW(REPLACE_EXISTING \| WRITE_THROUGH)` |

### 2.2 为什么用行式 `key=value` 而不是 JSON

- 两端都由本项目控制，JSON 换不来任何东西；
- 解析错误能精确到第几行（"第 3 行缺少 '='"），JSON 的报错位置往往没用；
- **末尾写一行 `complete=1`** 就能检测"被截断但恰好断在行边界"这种最阴的情况 ——
  JSON 里要发现这个得额外算校验和。

### 2.3 越界取值不静默纠正

音量写成了 250 时，夹到 100 并在 `notes` 里记一条：

```
bgm_volume=250 超出 0..100，已夹到 100
```

调用方应当把它写进日志。**静默纠正也是一种静默回退**，只是后果比静默失败轻。

### 2.4 未知键要跳过而不是判损坏

否则"新版本写过文件、又退回旧版本运行"会直接把用户设置判成损坏。
这条有专门的用例。

---

## 3. 单实例守卫

命名互斥体 + 一块共享内存携带已有进程的 PID。

**为什么不能只用互斥体**：它只能告诉你"已经有一个在跑"，拿不到 PID，
也就没法把那个窗口叫到前面来 —— 而"再双击一次应该恢复已有面板"正是要的行为。

名字用 `Local\` 前缀（同一用户会话内唯一，不跨会话抢）并带固定 GUID。

`WAIT_ABANDONED` 按"确实发生过异常，但我们可以继续启动"处理 ——
上一个持有者崩溃退出时互斥体会变成废弃状态，这时不该拦着自己启动。

---

## 4. 崩溃转储

`SetUnhandledExceptionFilter` + `MiniDumpWriteDump(MiniDumpNormal)`。
只留线程栈与模块列表，**刻意不用 `WithFullMemory`** —— 那既是几百 MB，
也会把用户的隐私数据整块带走。

写不出转储时，在磁盘上留一个明确的 `.dump-failed.txt` 说明失败原因，
而不是什么都不说。

两条实现约束：

1. **过滤器跑在一个已经出问题的进程里。** 所以目录路径在 `initialize` 时就拷进静态缓冲，
   崩溃路径上不做任何 C++ 容器分配；时间用 `GetLocalTime` 而不是 `localtime_s`。
2. **不尝试带病恢复。** 过滤器返回 `EXCEPTION_EXECUTE_HANDLER`，进程就退出。

转储不含完整内存，但仍可能含线程栈与少量敏感片段 —— 这件事必须告诉用户，
已在 README 里写明，不能默默收集。

---

## 5. 验收数据

```
$ ctest --preset release
1/7 core_tests                Passed
2/7 settings_tests            Passed
3/7 single_instance_tests     Passed
4/7 crash_reporter_tests      Passed
5/7 dpi_tests                 Passed
6/7 container_probe_tests     Passed
7/7 decoder_tests             Passed
100% tests passed out of 7
```

分项：

| 测试 | 断言数 | 覆盖 |
| --- | --- | --- |
| `settings_tests` | 88/88 | 五种损坏形态、迁移、原子保存、越界纠正、转义往返、未知键 |
| `single_instance_tests` | 18/18 | 跨进程识别 + PID 携带 + 持有者退出后释放 |
| `crash_reporter_tests` | 14/14 | 真崩一次 + 校验转储文件头 |

### 5.1 五种损坏形态都验证了隔离

```
bad_header       第 1 行不是预期的文件头
no_equals        第 3 行缺少 '='
truncated        文件缺少结束标记 complete=1，判断为被截断
bad_integer      键 bgm_volume 的值不是合法整数: abc
future_schema    文件 schema=999 高于本程序支持的 1，可能来自更新的版本
```

每一种都断言了四件事：状态是 `corrupt_isolated`、从默认值开始、原文件被挪走、
**隔离文件的内容与原文一字不差**（没有清空）。

### 5.2 跨进程验证单实例

这一条必须跨进程才有意义。测试自己起一个子进程持有守卫：

```
PASS 子进程在 5 秒内报告已持有守卫
PASS 第二个实例被识别为 already_running
PASS 共享内存里携带的是那个子进程的 PID
PASS 拿到的不是自己的 PID（否则等于没检测到）
PASS 持有者退出后守卫被释放（可以重新启动）
```

还有一条专门记录事实的用例：**同一线程重复获取命名互斥体会成功**，
所以同进程内根本测不出单实例。写下来免得以后有人用同进程用例来"验证"这件事。

### 5.3 真的崩一次

```
$ crash_reporter_tests.exe
child: 即将触发未处理异常
PASS 崩溃的子进程以非零码退出
PASS 崩溃后目录里留下了报告
PASS 写出的是转储文件（.dmp）
PASS 转储文件非空（大于 4KB）
PASS 转储文件头是 MDMP（是 Windows 能打开的小型转储）
```

`MDMP` 文件头这一条是有意加的：只检查"文件存在"是不够的，
空文件或写坏的半截文件都能通过那个断言。

---

## 6. 过程中发现并修掉的问题

### 6.1 `schema` 从来没被读进来（真 bug）

`parse_file` 把键值对填进了 `values`，但**没有把 `schema` 的值填进 `ParsedFile.schema`**。
后果是版本号恒为 0，所有文件都被当成"旧版本待迁移"。

测试立刻抓住了它 —— 11 条断言同时失败，根因就这一处。
这也说明"版本迁移"这种功能的测试值得写细：它错了不会崩，只会静默地把设置读成默认值。

### 6.2 单实例测试自己有竞态（测试设计问题）

第一版测试里，父进程一边轮询探测、子进程一边尝试获取同一把互斥体。
子进程可能正好在父进程持有它的那一瞬间去获取、然后失败退出 —— 而那个"别人"是父进程自己。

现象是子进程打印"期望拿到守卫，实际 already_running"，而父进程观察到 PID=0。

修法是加一个**就绪事件**握手：子进程拿到守卫后 `SetEvent`，父进程 `WaitForSingleObject`
之后才开始探测。这也解释了为什么"看起来像产品 bug"的现象实际是测试设计问题 ——
**验证手段本身也需要被怀疑**。

### 6.3 `MiniDumpWriteDump` 收非 const 指针

```cpp
const MINIDUMP_EXCEPTION_INFORMATION dump_info{...};
MiniDumpWriteDump(..., &dump_info, ...);   // 编译不过：转换丢失限定符
```

去掉 `const` 即可。

---

## 7. 已知未覆盖

- **没有验证"转储写不出来时留失败标记"这条路径。** 要构造它得让 `CreateFileW` 或
  `MiniDumpWriteDump` 失败（比如目录只读、磁盘满），测试里没有造这种环境。
  代码里有这条路径，但没有实测数据。
- **没有验证崩溃发生在别的线程。** 测试里的崩溃在主线程，且 `MINIDUMP_EXCEPTION_INFORMATION`
  里填的是 `GetCurrentThreadId()`。子线程崩溃的路径没测。
- **设置存储没有并发保护。** 现在假定只有一个进程在读它。单实例守卫保证了这一点，
  但守卫没生效时（比如用户手工删了互斥体名字）两份实例会互相覆盖设置。
- **没有验证磁盘满 / 只读目录下的保存失败。** `save_atomic` 会返回 false 并带错误码，
  但没有实测。
- **隔离文件不会自动清理。** 反复损坏会累积 `.corrupt-*` 文件，目前没有上限或清理策略。

---

## 8. 复现方式

```bash
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release --output-on-failure

# 单独跑某一项
./build/windows-x64/Release/settings_tests.exe
./build/windows-x64/Release/single_instance_tests.exe
./build/windows-x64/Release/crash_reporter_tests.exe

# 转储会落在测试用的临时目录里
ls "$TEMP/musicboard_crash_tests/"
```
