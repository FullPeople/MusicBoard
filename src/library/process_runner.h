#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 子进程运行器（非阻塞、可取消、能连坐子进程树）
//
// 存在的理由就是原版那两个具体问题：
//
//   1. **取消是假的。** 原版 `cancel_import()` 只把 `is_importing` 置回 false
//      （url_importer.gd:145），而 `OS.execute` 还在那个线程上阻塞着跑完；
//      随后 `_handle_thread_result` 照样发"下载完成"。更糟的是标志位一放，
//      用户就能再点一次导入 —— 两个后台线程同时写同一个 `_thread_result`。
//      这里用 Job Object + TerminateJobObject 真正杀掉进程树
//      （yt-dlp 会自己起 ffmpeg，只杀它自己会留下一个孤儿 ffmpeg）。
//
//   2. **进度不动。** 原版把 yt-dlp 的输出一次性收齐，
//      下完之后才从整段文本里解析出最终百分比。这里管道是**增量**读的：
//      每次 pump 取走已经到达的字节，调用方因此能在下载过程中真正刷新进度。
//
// 非阻塞是刻意的：pump() 一秒都不等，直接放进 UI 的消息循环里，
// 不需要为下载再开一个线程。
//
// 一个已知取舍：stderr 并进 stdout 用的是同一个管道，所以两者在极端情况下
// 会交错。好处是只有一个管道要读；代价是"错误行"理论上可能被进度行的残尾粘上。
// yt-dlp 加 --newline 之后每行都以 \n 结尾，实测没有出现粘行。
// ============================================================================

struct ProcessRunOptions final {
    std::filesystem::path executable;
    // 参数按"一个元素一个参数"给，引号由这里统一处理
    std::vector<std::string> arguments;
    // 可空。yt-dlp 的输出模板最好用绝对路径，所以这个字段不常用。
    std::filesystem::path working_directory;
    // 多久没有任何新输出就判定卡死。<= 0 表示不判定。
    double stall_timeout_seconds = 120.0;
    // 累积输出上限。超过之后继续读（不能让它写阻塞）但不再累积。
    std::size_t maximum_output_bytes = 4u * 1024u * 1024u;
};

enum class ProcessState : std::uint8_t {
    idle,
    running,
    // 正常结束（退出码可能非 0 —— 那是子进程的结论，不是运行器的问题）
    exited,
    // 被 cancel() 杀掉
    cancelled,
    // 运行器自己判定失败：启动不了、卡死、读管道出错
    failed,
};

[[nodiscard]] const char* process_state_name(ProcessState state) noexcept;

class ProcessRunner final {
public:
    ProcessRunner() = default;
    ~ProcessRunner();

    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    // 启动。成功时 state() == running。
    [[nodiscard]] bool start(const ProcessRunOptions& options, Failure& failure);

    // 泵一次：读走管道里已有的字节，检查退出与卡死。**不阻塞、不等。**
    // 所以它可以放心地放进 UI 的消息循环里，不需要为下载另开线程。
    //
    // 卡死判定用的是运行器内部的真实单调时钟（绝对时间戳相减，见边界 3），
    // 不接受调用方注入的时间 —— 一个"调用方给时间、内部却用真实时间"的接口
    // 只会两边对不上。测试要用很短的 timeout 并真的等一小会儿。
    void pump();

    // 杀死整棵进程树。幂等，任何状态都能调。
    // 进程已经自然结束时调用它不会把 exited 改写成 cancelled ——
    // 那样会让"下载完了"变成"被取消了"。
    void cancel();

    [[nodiscard]] ProcessState state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t exit_code() const noexcept { return exit_code_; }
    [[nodiscard]] const std::string& captured_output() const noexcept { return output_; }
    // state() == failed 时的一句原因（已含足够上下文）
    [[nodiscard]] const std::string& failure_reason() const noexcept { return failure_reason_; }
    [[nodiscard]] bool output_truncated() const noexcept { return output_truncated_; }

    // 取走 pump 之后新出现的**完整行**。行尾的 \r\n 或 \n 已经去掉。
    // 最后一次 pump 之后可能残留一个没有换行的尾巴，用 flush_tail_line() 取。
    [[nodiscard]] std::vector<std::string> take_new_lines();
    [[nodiscard]] std::string flush_tail_line();

    // 距离上一次有新输出过了多久。用于卡死判定与界面提示。
    [[nodiscard]] double idle_seconds() const noexcept;

    // Job Object 没能建起来或分配失败时的说明。
    // 这时取消只会 TerminateProcess，**孙进程可能活下来** ——
    // 不藏起来，因为那正是"取消是假的"的另一种形态。
    [[nodiscard]] const std::string& containment_note() const noexcept { return containment_note_; }

private:
    void read_available();
    void note_exit();
    void release_handles() noexcept;

    void* process_ = nullptr;      // HANDLE
    void* thread_ = nullptr;       // HANDLE
    void* job_ = nullptr;          // HANDLE
    void* read_pipe_ = nullptr;    // HANDLE
    void* write_pipe_ = nullptr;   // HANDLE（子进程侧，启动后父进程要关掉）

    ProcessState state_ = ProcessState::idle;
    std::uint32_t exit_code_ = 0;
    std::string output_;
    std::string failure_reason_;
    std::string containment_note_;
    std::size_t consumed_bytes_ = 0;   // output_ 里已经作为"完整行"交出去的部分
    bool output_truncated_ = false;
    bool pipe_closed_ = false;
    std::chrono::steady_clock::time_point last_output_at_{};
    double stall_timeout_seconds_ = 0.0;
    std::size_t maximum_output_bytes_ = 0;
};

}  // namespace musicboard
