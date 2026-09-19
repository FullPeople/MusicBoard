#include "core/crash_reporter.h"

#include <dbghelp.h>

#include <cstdio>
#include <cstring>

#include "core/path_utf8.h"

#pragma comment(lib, "dbghelp.lib")

namespace musicboard {
namespace {

// 崩溃路径上不能依赖 C++ 对象，所以这些是裸的静态量。
wchar_t g_dump_directory[kCrashPathCapacity]{};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
bool g_installed = false;
CrashReportOutcome g_last_outcome{};

// 只做字符串拼接，不做分配。返回是否成功。
[[nodiscard]] bool build_path(wchar_t (&out)[kCrashPathCapacity], const wchar_t* suffix) {
    if (g_dump_directory[0] == L'\0') {
        return false;
    }
    SYSTEMTIME local{};
    ::GetLocalTime(&local);

    wchar_t name[128]{};
    // 文件名带上时间，崩溃多次不会互相覆盖
    const int written = ::swprintf_s(name, L"crash-%04u%02u%02u-%02u%02u%02u-%lu%s",
                                     local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
                                     local.wSecond, ::GetCurrentProcessId(), suffix);
    if (written <= 0) {
        return false;
    }
    const int combined = ::swprintf_s(out, kCrashPathCapacity, L"%s\\%s", g_dump_directory, name);
    return combined > 0;
}

// 写一个明确的失败标记。内容里说明"转储为什么没写出来"。
void write_failure_marker(const wchar_t* path, DWORD reason) {
    const HANDLE handle = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    char line[256]{};
    const int length = std::snprintf(
        line, sizeof(line),
        "MusicBoard 未能写出崩溃转储。\r\n转储失败原因（Win32 错误）: %lu\r\n"
        "进程仍在崩溃退出中；本文件的存在本身就说明这一情况。\r\n",
        static_cast<unsigned long>(reason));
    if (length > 0) {
        DWORD written = 0;
        ::WriteFile(handle, line, static_cast<DWORD>(length), &written, nullptr);
    }
    ::CloseHandle(handle);
}

// 顶层未处理异常过滤器。
// 注意：这里运行在已经出问题的进程里，只做最必要的事，不做任何恢复尝试。
LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) {
    wchar_t dump_path[kCrashPathCapacity]{};
    if (build_path(dump_path, L".dmp")) {
        const HANDLE file = ::CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            // MiniDumpNormal：线程栈 + 模块列表。刻意不用 WithFullMemory ——
            // 全内存转储既是几百 MB，也会把用户的隐私数据整块带走。
            // MiniDumpWriteDump 收的是非 const 指针，这里不能用 const
            MINIDUMP_EXCEPTION_INFORMATION dump_info{::GetCurrentThreadId(), info, FALSE};
            const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file,
                                                MiniDumpNormal, &dump_info, nullptr, nullptr);
            const DWORD dump_error = ok ? ERROR_SUCCESS : ::GetLastError();
            ::CloseHandle(file);

            if (ok) {
                std::wmemcpy(g_last_outcome.path, dump_path, kCrashPathCapacity);
                g_last_outcome.dump_written = true;
                g_last_outcome.win32_error = ERROR_SUCCESS;
                return EXCEPTION_EXECUTE_HANDLER;
            }
            // 转储写不出来 —— 至少留一个明确标记，而不是什么都不说
            wchar_t marker_path[kCrashPathCapacity]{};
            if (build_path(marker_path, L".dump-failed.txt")) {
                write_failure_marker(marker_path, dump_error);
                std::wmemcpy(g_last_outcome.path, marker_path, kCrashPathCapacity);
            }
            g_last_outcome.dump_written = false;
            g_last_outcome.win32_error = dump_error;
            return EXCEPTION_EXECUTE_HANDLER;
        }
        write_failure_marker(dump_path, ::GetLastError());
        g_last_outcome.win32_error = ::GetLastError();
    }

    // 连目录都没有：把结论留在内存里，什么都不写也不能假装成功
    g_last_outcome.dump_written = false;
    g_last_outcome.win32_error = ERROR_PATH_NOT_FOUND;
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

CrashReporter::~CrashReporter() = default;

bool CrashReporter::initialize(const std::filesystem::path& directory, Failure& failure) {
    if (g_installed) {
        failure.set(fail_code::invalid_argument, "崩溃上报已经装过一次");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        failure.set(fail_code::file_open_failed,
                    "无法创建崩溃转储目录 " + path_to_utf8(directory) + ": " + ec.message());
        return false;
    }

    // 把目录拷贝进静态缓冲。崩溃路径上不再碰 std::filesystem。
    const std::wstring wide = directory.wstring();
    if (wide.size() + 1 > kCrashPathCapacity) {
        failure.set(fail_code::invalid_argument,
                    "崩溃转储目录路径过长（超过 " + std::to_string(kCrashPathCapacity) + " 个宽字符）");
        return false;
    }
    std::wmemcpy(g_dump_directory, wide.c_str(), wide.size());
    g_dump_directory[wide.size()] = L'\0';

    g_previous_filter = ::SetUnhandledExceptionFilter(&unhandled_exception_filter);
    g_installed = true;
    return true;
}

void CrashReporter::shutdown() noexcept {
    if (!g_installed) {
        return;
    }
    ::SetUnhandledExceptionFilter(g_previous_filter);
    g_previous_filter = nullptr;
    g_dump_directory[0] = L'\0';
    g_installed = false;
}

bool CrashReporter::installed() const noexcept {
    return g_installed;
}

const CrashReportOutcome& CrashReporter::last_outcome() noexcept {
    return g_last_outcome;
}

}  // namespace musicboard
