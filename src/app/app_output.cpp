#include "app/app_output.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "core/path_utf8.h"
#include "core/product_settings.h"

namespace musicboard::app {
namespace {

std::mutex g_mutex;
std::ofstream g_log;
std::filesystem::path g_log_path;
bool g_has_console = false;

[[nodiscard]] std::string timestamp() {
    // 带毫秒。
    //
    // 秒级精度不够用：查"悬停为什么卡"时要看的是**相邻两次鼠标消息之间**的间隔，
    // 而那种卡顿在 100~300ms 量级 —— 秒级时间戳上完全看不出来（同一秒里挤几条
    // 看起来和流畅时一模一样）。加三位毫秒之后，"哪一段卡了、卡了多久"才有据可查。
    SYSTEMTIME local{};
    ::GetLocalTime(&local);
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
                  local.wSecond, local.wMilliseconds);
    return buffer;
}

[[nodiscard]] std::string date_stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (::localtime_s(&local, &now) != 0) {
        return "00000000";
    }
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday);
    return buffer;
}

}  // namespace

std::filesystem::path initialize_output() {
    // 1) 输出到哪里去。
    //
    // GUI 子系统的进程默认没有控制台，所以 `--version` 之类的输出会凭空消失。
    // 两种情况都要照顾：
    //
    //   a) 父进程是个控制台（cmd / PowerShell）→ AttachConsole 附上去，
    //      必要时把 stdout 指到 CONOUT$。
    //   b) 父进程给的是管道或文件（Git Bash / mintty、重定向、IDE 捕获）→
    //      **标准句柄本身是有效的**，什么都不用做，printf 照常写进去。
    //
    // (b) 这一条是实测补上的：在 mintty 里跑 --selftest 什么都没有，
    // 因为 mintty 不分配 Windows 控制台，而代码只在 (a) 成功时才认为有输出。
    const HANDLE existing_stdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const bool stdout_usable =
        existing_stdout != nullptr && existing_stdout != INVALID_HANDLE_VALUE;

    if (::AttachConsole(ATTACH_PARENT_PROCESS) != FALSE) {
        g_has_console = true;
        if (!stdout_usable) {
            FILE* dummy = nullptr;
            static_cast<void>(freopen_s(&dummy, "CONOUT$", "w", stdout));
            static_cast<void>(freopen_s(&dummy, "CONOUT$", "w", stderr));
        }
    } else if (stdout_usable) {
        g_has_console = true;
    }

    // 2) 同时开一份日志文件。**双击启动时这是唯一的现场**。
    const std::filesystem::path root = product_data_root();
    if (!root.empty()) {
        std::error_code ec;
        const std::filesystem::path directory = root / L"logs";
        std::filesystem::create_directories(directory, ec);
        if (!ec) {
            // 注意：date_stamp() 只能调**一次**。
            // 写成 `std::wstring(date_stamp().begin(), date_stamp().end())` 会构造出
            // 一个 [临时对象1 的 begin, 临时对象2 的 end) 的区间 —— 两个不同的临时对象，
            // 那是未定义行为（实测：文件名变成垃圾，日志文件根本开不出来）。
            const std::string stamp = date_stamp();
            const std::wstring wide_stamp(stamp.begin(), stamp.end());
            g_log_path = directory / (std::wstring(L"app-") + wide_stamp + L".log");
            g_log.open(g_log_path, std::ios::binary | std::ios::app);
            if (g_log) {
                g_log << "---- 启动 " << timestamp() << " ----\n";
                g_log.flush();
            } else {
                g_log_path.clear();
            }
        }
    }
    return g_log_path;
}

void report_line(std::string_view text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_has_console) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    if (g_log) {
        g_log << text << '\n';
        g_log.flush();
    }
}

void report_line_fmt(const char* format, ...) {
    char buffer[2048]{};
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (written < 0) {
        return;
    }
    std::string_view text(buffer);
    // 原来的 printf 自带换行；report_line 也会补一个，去掉一个免得空行
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    report_line(text);
}

void report(LogLevel level, std::string_view event, std::string_view context) {
    std::string line = timestamp();
    line += "  [";
    switch (level) {
        case LogLevel::info:
            line += "info";
            break;
        case LogLevel::warning:
            line += "warn";
            break;
        case LogLevel::error:
            line += "error";
            break;
    }
    line += "] ";
    line += event;
    if (!context.empty()) {
        line += ": ";
        line += context;
    }
    report_line(line);
}

const std::filesystem::path& output_log_path() noexcept {
    return g_log_path;
}

}  // namespace musicboard::app
