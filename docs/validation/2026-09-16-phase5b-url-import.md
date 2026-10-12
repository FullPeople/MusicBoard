# 阶段 5 · URL 导入（真实进度、真取消、不猜文件名）

日期：2026-09-16
范围：`src/library/url_classify`、`process_runner`、`staging`、`import_job`、
`http_download`、`tests/fake_child.cpp`、`tests/test_http_server.h`、
四个新测试可执行。
对应正本：`docs/IMPLEMENTATION.zh-CN.md` 阶段 5。
上一份记录：[曲库层：扫描、文件操作、导入事务](2026-09-16-phase5-library.md)。

## 一句话结论

URL 导入（yt-dlp 路线与直链路线）已经全部接上并通过测试。这一层存在的理由是原版
`url_importer.gd` 的四个具体问题，**每一条都有一条测试钉住**，
而且整个过程不碰网络：yt-dlp 路线靠一个行为可控的假下载器当子进程，
直链路线靠一个跑在 127.0.0.1 上的回环 HTTP 服务器。

## 测试结果（Release）

```
core_tests                33/33
settings_tests            88/88
single_instance_tests     18/18
crash_reporter_tests      14/14
dpi_tests                 30/30
container_probe_tests     20/20
voice_timeline_tests      49/49
audio_engine_tests        77/77
decoder_tests             21/21
library_ops_tests        111/111
import_transaction_tests  52/52   （1 条 SKIP）
library_scanner_tests     68/68
url_classify_tests        72/72
http_download_tests       57/57
process_runner_tests      53/53
import_job_tests         132/132
```

`ctest --preset release` 与 `--preset debug`：均为 16/16 通过（29.85 秒 / 41.85 秒）。
Debug 与 Release 都是 `/W4 /WX` 零警告。

真实曲库（Release）：491 个候选、491 成功、0 失败、20 个 BGM 分类、2 个 SFX 分类、
**2.09 秒、工作集增量 0.0 MB**。证据：`evidence/library-scan-real.txt`。

## 原版的四个问题，以及各自的对策

### 1. 取消是假的

原版 `cancel_import()` 只把 `is_importing` 置回 false（`url_importer.gd:145`），
而 `OS.execute` 还在那个线程上阻塞着跑完；随后 `_handle_thread_result` 照样发出
"下载完成"。更糟的是标志位一放，用户就能再点一次导入 ——
两个后台线程同时写同一份 `_thread_result`。

对策：`ProcessRunner` 把子进程放进一个 Job Object（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`），
取消时 `TerminateJobObject`。**这一条不是洁癖**：yt-dlp 会自己起 ffmpeg，
只杀 yt-dlp 会留下一个孤儿 ffmpeg 继续把 CPU 吃满。

取消之后 `cancel()` 会等最多 5 秒并复查退出码；如果进程还活着（`STILL_ACTIVE`），
它会明确写下"已经请求取消，但子进程在 5 秒内没有退出"。
测试断言取消后这句话必须为空 —— 也就是"进程真的死了"。

### 2. 进度永远不动

原版只在开始（0.0）和结束（1.0）各发一次 `import_progress`，
中间的百分比是下完之后才从整段输出里抠出来的。进度条不可能动。

对策：管道是**增量**读的（`PeekNamedPipe` 看有没有字节，再读走），
所以 `pump()` 一秒都不等，可以直接放进 UI 的消息循环里。
`ImportJobStatus.fraction` 在下载过程中真的在变 ——
测试断言"观察到至少 4 次进度变化，且其中至少一次严格位于 0 和 1 之间"。

一个细节：yt-dlp 在合并/转码阶段会重新从 0 开始报百分比，
所以进度只许往前走。测试断言进度序列单调不减。

### 3. 猜文件名

原版让下载器**直接写进曲库目录**，然后用 `_find_newest_file()`
按修改时间猜哪个是刚下的（`url_importer.gd:449`）。两个后果：
下载中途失败会在曲库里留下半个文件（界面上它跟正常曲目长得一样）；
"最新那个文件"完全可能不是刚下的。

对策：
- 每个导入任务一个**空的**暂存子目录（`staging/job-<时间>-<pid>-<序号>`）；
- 下完之后**列目录**，不猜。恰好 1 个文件 → 走 `import_staged_file` 事务发布；
  0 个 → 明确失败（"yt-dlp 报告成功，但暂存目录里没有任何文件"）；
  2 个及以上 → 明确失败，并把实际看到的文件名都列出来
  （挑一个就是猜，猜错比报错更糟）；
- 产物必须先过 `probe_audio_file` 才能进曲库。所以"改名成 .mp3 的文本文件"
  会让整次导入失败，而曲库依然是空的 —— 有一个专门的用例（`junk` 模式）钉住它。

`-o` 模板用的是**绝对路径**指向暂存目录，不依赖子进程的工作目录。

### 4. 错误信息只写控制台

原版把 yt-dlp 的完整输出 `print_rich` 到控制台，界面上只给一句
十几种判据拼出来的提示。

对策：`extract_yt_dlp_error` 挑出最有意义的那一行（优先带 `error` 的行，
否则取最后一行非空输出），`suggest_yt_dlp_fix` 给一句可执行的建议。
两者都是纯函数，用真实的 yt-dlp 错误文本穷举测过。
测试断言 403 那一条给出的建议里必须出现"拒绝" ——
也就是"建议和原因必须对得上"，而不是随便给一条。

## 测试怎么做到不碰网络

`tests/fake_child.cpp` 是一个行为可控的假下载器。它有两种调用方式：

1. **直接模式**：`argv[1]` = 模式，`argv[2]` = 目录。`process_runner_tests` 用这个，
   精确控制输出时序（"分 10 次报进度""一个字都不打""永远不退出"）。
2. **yt-dlp 模式**：设了环境变量 `MUSICBOARD_FAKE_MODE`，
   参数表则是 `import_job` 拼出来的**真实 yt-dlp 参数**。它从 `-o` 模板里
   解析出该写到哪个目录。

第二种方式的价值不只是"省一个测试钩子"：它把 `import_job` 拼出来的参数表
真的交给了子进程去用。**引号、路径、`-o` 模板任何一处拼错，这些用例会直接红，
而且生产代码里不需要留任何测试专用的入口。** 参数表本身另有一组断言
（`--no-playlist`、`--newline`、`-o` 是绝对路径、URL 在最后）。

模式清单：`success` / `progress` / `fail` / `refusal` / `silent` / `hang` /
`junk` / `empty` / `two_files` / `spam` / `echo_args`。
每一种都对应一个真实会发生的场景。

## 测试抓出来的东西

1. **搜索结果里少一步就永远命中的坑**：先写的测试断言失败原因里含"没有输出"，
   而实际消息是"没有**任何**输出"。断言本身写错了 —— 这类错误如果只靠
   "看一遍代码觉得对"是抓不到的。
2. **子进程的 stdout 是文本模式，CRT 会把 `\n` 写成 `\r\n`**。
   在 `captured_output()` 里直接 `find("ARG: \n")` 永远找不到。
   改成比较拆好的行（顺便也测了行拆分）。
3. **`stem_from_url("https://example.com/")` 推出 "example"** ——
   最后一"段"其实是域名。原版的 `_extract_title_from_url` 有同样的问题
   （它会一路往前找非空段）。改成"主机名后面必须真的有路径"。
4. **进度解析会把 "-5%" 读成 "5%"**：往后退着扫数字只会在 '-' 处停下，
   于是负进度被当成 5% 放过去。改成"数字前面带符号就不认"。
5. **`resolve_yt_dlp` 里把 `path_from_utf8` 当构造函数用**：那个函数返回 bool
   并填出参，写成 `std::filesystem::path(path_from_utf8(...))` 是错的
   （而且会被"值语义"悄悄编译过）。抽成 `path_of_utf8` 才看得清。

## 刻意的取舍与仍然存在的边界

**一个 Job Object 分配失败时的降级是"说出来"而不是"装作没事"。**
在某些受限环境里 `AssignProcessToJobObject` 会失败。这时取消只能
`TerminateProcess`，**孙进程可能活下来** —— 这一条写进 `containment_note()`，
不藏起来，因为那正是"取消是假的"的另一种形态。

**卡死判定用的是运行器内部的真实单调时钟，不接受调用方注入时间。**
一个"调用方给时间、内部却用真实时间"的接口只会两边对不上。
所以进程相关的测试真的会等，超时都设得很短。

**`start()` 里先判路线、再建暂存目录。** 顺序反过来的话，
一个注定要拒绝的链接也会在磁盘上留下一个空目录。有两条用例专门断言
"被拒之后暂存区是空的"。

**有一处当前行为不够理想，照实记下来**：`yt_dlp_path` 指向一个不存在的文件时，
暂存目录已经建出来了（建目录在启动子进程之前）。它会被保留 ——
一个空目录不影响任何东西，下次启动的 `prune_staging` 会收走它。
测试里注明了这一点，没有把它写成一个更好听的样子。

## 未完成

本阶段已经没有未完成的下载路线。剩下两件事属于后续阶段：

- **阶段 3.5**：悬浮球的分层窗口（`UpdateLayeredWindow`）。
- **阶段 6**：UI。现在这些能力都还只有库和测试，没有一处被界面用上 ——
  `ImportJobStatus` 已经把界面需要的字段都备齐了
  （`fraction` / `received_bytes` / `has_total` / `status_text` /
  `failure_reason` / `suggestion` / `staging_dir`）。

两个产品决定待定（不影响本阶段）：SFX 台位固定 4 个还是扩到 6/8；
是否做全局快捷键。

---

## 补充：直链下载（`direct_http`）

上面记的是 yt-dlp 路线完成时的状态。直链路线随后也接上了，
实现是 `src/library/http_download.*`，测试是 `tests/http_download_tests`
与 `import_job_tests` 里的直链段，用一个跑在 127.0.0.1 上的回环 HTTP 服务器
（`tests/test_http_server.h`）驱动 —— 仍然不碰公网。

回环服务器能造出来的场景，以及各自钉住的东西：

| 场景 | 钉住的是什么 |
| --- | --- |
| 有 `Content-Length` + 分片慢发 | 百分比是真的，且下载**过程中**就能观察到变化 |
| 不给 `Content-Length` | `has_total` 为假、`fraction` 恒为 0 —— **不编分母** |
| HTTP 404 / 500 | 状态码原样回报，原因里带数字，404 的建议是"已被删除" |
| 声明了长度却提前掐断连接 | 明确失败（"传输不完整：声明 N 字节，实际收到 M 字节"） |
| 200 但 0 字节 | 明确失败 —— 很多站点用"空体 + 200"表示"你需要登录" |
| 慢速下载中途取消 | 真的停下（字节数不再增长），暂存目录被清掉 |
| 连不上的主机 | 明确失败，不会一直挂着 |

**最要紧的一条：扩展名按嗅探结果，而不是照 URL 或服务器说的。**
用例里服务器返回的是 WAV，但 URL 与 `Content-Disposition` 都声称是 `.mp3` ——
入库后的文件名必须是 `.wav`，主干用服务器给的 `server_says_mp3`。
理由与解码层的第 5 条边界是同一条：扩展名是用户随手能改的东西。

这一条也顺带抓出一个真 bug：`publish_from_staging` 会把
`request_.preferred_stem`（直链路线下已经被我们从 URL 推过的值）传下去，
把改名那一步刚定好的好名字又压回成 `actually_wav`。
修法是只把**用户明确给过的**名字传下去（`preferred_stem_from_user_`）。

另外两处也是这一轮抓出来的：

- `HttpDownload::cancel()` 改的是原子状态，但 `status()` 是 `pump()` 时快照的，
  于是"取消了，状态却显示还在下"。现在 `cancel()` 结束时也搬一次计数。
- `WinHttpQueryHeaders` 用 `WINHTTP_QUERY_CUSTOM` 单个查 `Content-Disposition`
  查不到（不报错、只是返回 FALSE），于是服务器给的好名字被静默丢掉。
  改成一次拉整段原始响应头（`WINHTTP_QUERY_RAW_HEADERS_CRLF`）自己按行找，
  行为完全在本地。**这类"官方 API 静默不返回值"的地方，测试是唯一的发现手段** ——
  纯函数那 6 条断言全过，问题在它和 WinHTTP 之间。
