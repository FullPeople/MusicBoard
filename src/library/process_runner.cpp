#include "library/process_runner.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include "core/path_utf8.h"

namespace musicboard {
namespace {

constexpr DWORD kStillActive = 259;   // STILL_ACTIVE

// 按 Windows 的命令行规则给一个参数加引号。
//
// 规则比"前后加双引号"复杂：反斜杠只有在双引号之前才需要转义，
// 否则 Windows 会把它当成转义符吃掉（经典例子：路径以 \ 结尾时
// C:\dir\ 会变成 C:\dir"）。
[[nodiscard]] std::wstring quote_argument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    const bool needs_quotes =
        argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) {
        return argument;
    }

    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            // 引号之前的反斜杠要翻倍，再加一个转义引号本身
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    // 结尾的反斜杠也要翻倍，否则会去转义我们刚加上的收尾引号
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

[[nodiscard]] std::string hresult_text(std::string_view what, DWORD error) {
    return std::string(what) + "（Win32 错误 " + std::to_string(error) + "）";
}

}  // namespace

const char* process_state_name(ProcessState state) noexcept {
    switch (state) {
        case ProcessState::idle:
            return "idle";
        case ProcessState::running:
            return "running";
        case ProcessState::exited:
            return "exited";
        case ProcessState::cancelled:
            return "cancelled";
        case ProcessState::failed:
            return "failed";
    }
    return "unknown";
}

ProcessRunner::~ProcessRunner() {
    // 顺序要紧：先杀进程树（KILL_ON_JOB_CLOSE 也会兜住），再关句柄。
    if (job_ != nullptr && state_ == ProcessState::running) {
        ::TerminateJobObject(static_cast<HANDLE>(job_), 1);
    }
    release_handles();
}

void ProcessRunner::release_handles() noexcept {
    const auto close = [](void*& handle) {
        if (handle != nullptr) {
            ::CloseHandle(static_cast<HANDLE>(handle));
            handle = nullptr;
        }
    };
    close(read_pipe_);
    close(write_pipe_);
    close(thread_);
    close(process_);
    // job 放最后：上面那几个都关掉之后，KILL_ON_JOB_CLOSE 才生效
    close(job_);
}

bool ProcessRunner::start(const ProcessRunOptions& options, Failure& failure) {
    if (state_ != ProcessState::idle) {
        failure.set(fail_code::invalid_argument, "这个运行器已经用过一次了，请新建一个");
        return false;
    }
    if (options.executable.empty()) {
        failure.set(fail_code::invalid_argument, "可执行文件路径为空");
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(options.executable, ec)) {
        failure.set(fail_code::file_not_found,
                    "找不到可执行文件: " + path_to_utf8(options.executable));
        return false;
    }

    stall_timeout_seconds_ = options.stall_timeout_seconds;
    maximum_output_bytes_ = options.maximum_output_bytes;

    // ---- 管道 ----
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;   // 让子进程能继承写端

    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (::CreatePipe(&read_handle, &write_handle, &attributes, 0) == FALSE) {
        failure.set(fail_code::platform_api_failed,
                    hresult_text("创建管道失败", ::GetLastError()));
        return false;
    }
    read_pipe_ = read_handle;
    write_pipe_ = write_handle;
    // 读端**不能**被继承 —— 否则子进程自己也持有读端，
    // 它退出后管道永远不会变成"已关闭"，我们会一直等下去。
    if (::SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        failure.set(fail_code::platform_api_failed,
                    hresult_text("设置管道读端不可继承失败", ::GetLastError()));
        release_handles();
        return false;
    }

    // ---- 命令行 ----
    std::wstring command_line;
    {
        std::wstring wide_executable;
        Failure conversion_failure;
        if (!utf8_to_wide(path_to_utf8(options.executable), wide_executable, conversion_failure)) {
            failure.set(fail_code::invalid_argument,
                        "可执行文件路径不是合法 UTF-8: " + conversion_failure.context);
            release_handles();
            return false;
        }
        command_line = quote_argument(wide_executable);
        for (std::size_t index = 0; index < options.arguments.size(); ++index) {
            const std::string& argument = options.arguments[index];
            std::wstring wide_argument;
            Failure argument_failure;
            if (!utf8_to_wide(argument, wide_argument, argument_failure)) {
                // 参数是用户给的曲名或 URL，来自 UTF-8 边界之内。
                // 不是合法 UTF-8 就明确报错，绝不"跳过这个参数"。
                failure.set(fail_code::invalid_argument,
                            "第 " + std::to_string(index + 1) + " 个参数不是合法 UTF-8: " +
                                argument);
                release_handles();
                return false;
            }
            command_line.push_back(L' ');
            command_line += quote_argument(wide_argument);
        }
    }

    std::wstring working_directory;
    if (!options.working_directory.empty()) {
        working_directory = options.working_directory.wstring();
    }

    // ---- 启动（挂起，好让 job 先分配上） ----
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    // stderr 并进同一个管道：只有一个管道要读，代价见头文件的说明
    startup.hStdOutput = static_cast<HANDLE>(write_pipe_);
    startup.hStdError = static_cast<HANDLE>(write_pipe_);
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION info{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, flags, nullptr,
                         working_directory.empty() ? nullptr : working_directory.c_str(), &startup,
                         &info) == FALSE) {
        failure.set(fail_code::platform_api_failed,
                    hresult_text("启动子进程失败: " + path_to_utf8(options.executable),
                                 ::GetLastError()));
        release_handles();
        return false;
    }
    process_ = info.hProcess;
    thread_ = info.hThread;

    // 父进程不再需要写端；留着它我们自己会让管道永远不"关闭"。
    ::CloseHandle(static_cast<HANDLE>(write_pipe_));
    write_pipe_ = nullptr;

    // ---- Job Object：保证取消能连坐 ffmpeg 这类孙进程 ----
    if (HANDLE job = ::CreateJobObjectW(nullptr, nullptr); job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                      sizeof(limits)) != FALSE &&
            ::AssignProcessToJobObject(job, static_cast<HANDLE>(process_)) != FALSE) {
            job_ = job;
        } else {
            const DWORD reason = ::GetLastError();
            containment_note_ =
                "进程树隔离没有建立起来（Win32 错误 " + std::to_string(reason) +
                "）。取消时只能杀 yt-dlp 自己，它启动的 ffmpeg 可能留下来继续跑。";
            ::CloseHandle(job);
        }
    } else {
        containment_note_ =
            "创建 Job Object 失败（Win32 错误 " + std::to_string(::GetLastError()) +
            "）。取消时只能杀 yt-dlp 自己，它启动的 ffmpeg 可能留下来继续跑。";
    }

    if (::ResumeThread(static_cast<HANDLE>(thread_)) == static_cast<DWORD>(-1)) {
        failure.set(fail_code::platform_api_failed,
                    hresult_text("恢复子进程线程失败", ::GetLastError()));
        ::TerminateProcess(static_cast<HANDLE>(process_), 1);
        release_handles();
        return false;
    }

    last_output_at_ = std::chrono::steady_clock::now();
    state_ = ProcessState::running;
    return true;
}

void ProcessRunner::read_available() {
    if (read_pipe_ == nullptr || pipe_closed_) {
        return;
    }
    std::array<char, 8192> buffer{};
    for (;;) {
        DWORD available = 0;
        if (::PeekNamedPipe(static_cast<HANDLE>(read_pipe_), nullptr, 0, nullptr, &available,
                            nullptr) == FALSE) {
            // 写端全部关闭 → 后面不会再有输出了
            const DWORD error = ::GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_INVALID_HANDLE) {
                pipe_closed_ = true;
            }
            return;
        }
        if (available == 0) {
            return;
        }

        DWORD read = 0;
        const DWORD want = std::min(available, static_cast<DWORD>(buffer.size()));
        if (::ReadFile(static_cast<HANDLE>(read_pipe_), buffer.data(), want, &read, nullptr) ==
                FALSE ||
            read == 0) {
            pipe_closed_ = true;
            return;
        }

        last_output_at_ = std::chrono::steady_clock::now();
        if (output_.size() >= maximum_output_bytes_) {
            // 继续读（不读会让子进程写阻塞），但不再累积。
            // 这不是静默：output_truncated() 会把这件事报出去。
            output_truncated_ = true;
            continue;
        }
        const std::size_t room = maximum_output_bytes_ - output_.size();
        const auto as_size = static_cast<std::size_t>(read);
        output_.append(buffer.data(), std::min(room, as_size));
        if (as_size > room) {
            output_truncated_ = true;
        }
    }
}

void ProcessRunner::note_exit() {
    DWORD code = 0;
    if (::GetExitCodeProcess(static_cast<HANDLE>(process_), &code) == FALSE) {
        state_ = ProcessState::failed;
        failure_reason_ = hresult_text("读取子进程退出码失败", ::GetLastError());
        return;
    }
    if (code == kStillActive) {
        return;
    }
    // 退出之后可能还有没读完的输出，再读一遍（写端这时已经关了）
    read_available();
    exit_code_ = code;
    state_ = ProcessState::exited;
}

void ProcessRunner::pump() {
    if (state_ != ProcessState::running) {
        return;
    }
    read_available();
    note_exit();
    if (state_ != ProcessState::running) {
        return;
    }

    if (stall_timeout_seconds_ > 0.0 && idle_seconds() > stall_timeout_seconds_) {
        failure_reason_ =
            "超过 " + std::to_string(static_cast<int>(stall_timeout_seconds_)) +
            " 秒没有任何输出，判定为卡死并已终止";
        cancel();
        state_ = ProcessState::failed;
    }
}

void ProcessRunner::cancel() {
    if (state_ != ProcessState::running) {
        return;
    }
    if (job_ != nullptr) {
        // 连坐：yt-dlp 起的 ffmpeg 一起杀掉
        ::TerminateJobObject(static_cast<HANDLE>(job_), 1);
    } else if (process_ != nullptr) {
        ::TerminateProcess(static_cast<HANDLE>(process_), 1);
    }
    if (process_ != nullptr) {
        ::WaitForSingleObject(static_cast<HANDLE>(process_), 5000);
    }
    DWORD code = 0;
    if (process_ != nullptr && ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code) != FALSE) {
        exit_code_ = code;
        if (code == kStillActive) {
            // 取消之后进程还活着，这是必须说出来的事实 ——
            // 原版的问题就是"以为取消了、其实还在下"。
            failure_reason_ = "已经请求取消，但子进程在 5 秒内没有退出";
        }
    }
    read_available();
    state_ = ProcessState::cancelled;
}

double ProcessRunner::idle_seconds() const noexcept {
    if (state_ != ProcessState::running) {
        return 0.0;
    }
    const auto elapsed = std::chrono::steady_clock::now() - last_output_at_;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    return seconds > 0.0 ? seconds : 0.0;
}

std::vector<std::string> ProcessRunner::take_new_lines() {
    std::vector<std::string> lines;
    std::size_t cursor = consumed_bytes_;
    std::size_t line_start = consumed_bytes_;
    while (cursor < output_.size()) {
        if (output_[cursor] == '\n') {
            std::size_t end = cursor;
            if (end > line_start && output_[end - 1] == '\r') {
                --end;
            }
            lines.emplace_back(output_, line_start, end - line_start);
            line_start = cursor + 1;
        }
        ++cursor;
    }
    consumed_bytes_ = line_start;
    return lines;
}

std::string ProcessRunner::flush_tail_line() {
    if (consumed_bytes_ >= output_.size()) {
        return {};
    }
    std::string tail = output_.substr(consumed_bytes_);
    consumed_bytes_ = output_.size();
    while (!tail.empty() && (tail.back() == '\r' || tail.back() == '\n')) {
        tail.pop_back();
    }
    return tail;
}

}  // namespace musicboard
