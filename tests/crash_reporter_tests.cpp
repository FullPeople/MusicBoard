// 崩溃转储测试
//
// 要验证的是"出了未处理异常之后，磁盘上留下了一份可诊断的现场"。
// 这件事没法在本进程里测（测完就没了），所以起一个子进程真的崩一次。
//
// 结构：
//   父进程（默认）        起子进程让它崩 → 检查转储目录里出现了 .dmp 或 .dump-failed.txt
//   子进程（--child-crash <dir>）  装上上报器，然后解引用空指针

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "core/crash_reporter.h"
#include "core/path_utf8.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

std::wstring executable_path() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD written = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return std::wstring(buffer, written);
}

std::filesystem::path temp_dir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path() / L"musicboard_crash_tests";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

std::filesystem::path case_dir(const char* name) {
    const std::filesystem::path dir = temp_dir() / std::wstring(name, name + std::strlen(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

struct FileCounts final {
    int dumps = 0;
    int failure_markers = 0;
    int total = 0;
};

FileCounts count_reports(const std::filesystem::path& dir) {
    FileCounts counts;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        ++counts.total;
        const std::string name = path_to_utf8(entry.path().filename());
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dmp") == 0) {
            ++counts.dumps;
        } else if (name.find("dump-failed") != std::string::npos) {
            ++counts.failure_markers;
        }
    }
    return counts;
}

// 起一个自己，让它崩溃。返回退出码。
[[nodiscard]] bool spawn_crashing_child(const std::filesystem::path& directory, DWORD& exit_code,
                                       Failure& failure) {
    const std::wstring exe = executable_path();
    if (exe.empty()) {
        failure.set(fail_code::platform_api_failed, "取不到自身可执行文件路径");
        return false;
    }

    std::wstring command_line = exe + L" --child-crash \"" + directory.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                          &startup, &info)) {
        failure.set(fail_code::platform_api_failed,
                    "CreateProcessW 失败 (Win32 错误 " + std::to_string(::GetLastError()) + ")");
        return false;
    }
    ::CloseHandle(info.hThread);
    ::WaitForSingleObject(info.hProcess, 30000);
    ::GetExitCodeProcess(info.hProcess, &exit_code);
    ::CloseHandle(info.hProcess);
    return true;
}

void test_crash_leaves_a_report(Registry& r) {
    const auto dir = case_dir("real_crash");
    r.check(count_reports(dir).total == 0, "崩溃前目录是空的");

    DWORD exit_code = 0;
    Failure failure;
    r.check_invocation([&] { return spawn_crashing_child(dir, exit_code, failure); }, "能起一个崩溃的子进程", failure);
    if (!failure.empty()) {
        return;
    }

    // 进程必须以非零码退出 —— 崩了却"正常退出"是最坏的情况
    r.check_eq(exit_code != 0, "崩溃的子进程以非零码退出",
               "退出码=" + std::to_string(exit_code));

    const FileCounts counts = count_reports(dir);
    r.check_eq(counts.total > 0, "崩溃后目录里留下了报告", "文件数=" + std::to_string(counts.total));
    r.check_eq(counts.dumps > 0, "写出的是转储文件（.dmp）",
               "转储=" + std::to_string(counts.dumps) +
                   " 失败标记=" + std::to_string(counts.failure_markers));

    // 转储体积要能看得出是个真东西 —— 空文件说明 MiniDumpWriteDump 什么都没写
    if (counts.dumps > 0) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const std::string name = path_to_utf8(entry.path().filename());
            if (name.find(".dmp") == std::string::npos) {
                continue;
            }
            const auto size = std::filesystem::file_size(entry.path(), ec);
            r.check_eq(!ec && size > 4096, "转储文件非空（大于 4KB）",
                       "大小=" + std::to_string(ec ? 0 : size) + " 字节");

            // 文件头应当是 MDMP
            const HANDLE handle = ::CreateFileW(entry.path().c_str(), GENERIC_READ, FILE_SHARE_READ,
                                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                char signature[4]{};
                DWORD read = 0;
                ::ReadFile(handle, signature, 4, &read, nullptr);
                ::CloseHandle(handle);
                r.check_eq(read == 4 && signature[0] == 'M' && signature[1] == 'D' &&
                               signature[2] == 'M' && signature[3] == 'P',
                           "转储文件头是 MDMP（是 Windows 能打开的小型转储）",
                           std::string("头 4 字节=") + std::string(signature, read));
            }
        }
    }
}

void test_initialize_creates_directory(Registry& r) {
    const auto base = case_dir("init");
    const auto nested = base / L"a" / L"b" / L"崩溃";   // 含中文的多级目录

    CrashReporter reporter;
    Failure failure;
    r.check_invocation([&] { return reporter.initialize(nested, failure); }, "初始化时自动创建转储目录", failure);
    r.check(reporter.installed(), "过滤器已装上");
    r.check(std::filesystem::is_directory(nested), "多级目录（含中文）已被创建");
    reporter.shutdown();
    r.check(!reporter.installed(), "shutdown 后不再处于已装状态");
}

void test_double_install_is_rejected(Registry& r) {
    const auto dir = case_dir("double");
    CrashReporter first;
    Failure failure;
    r.check(first.initialize(dir, failure), "第一次安装成功");

    CrashReporter second;
    Failure second_failure;
    r.check(!second.initialize(dir, second_failure), "重复安装被拒绝");
    r.check(second_failure.code == fail_code::invalid_argument, "重复安装给出 invalid_argument");
    first.shutdown();
}

}  // namespace

// 子进程：装上上报器，然后真的崩一次
int run_child_crash(const std::filesystem::path& directory) {
    CrashReporter reporter;
    Failure failure;
    if (!reporter.initialize(directory, failure)) {
        std::printf("child: 上报器初始化失败 %s\n", failure.to_string().c_str());
        return 2;
    }
    std::printf("child: 即将触发未处理异常\n");
    std::fflush(stdout);

    // 故意解引用空指针。不能优化掉，所以用 volatile 拦一道。
    volatile int* null_pointer = nullptr;
    *null_pointer = 1;

    // 走到这里说明没崩 —— 那本身就是失败
    std::printf("child: 竟然没崩\n");
    return 3;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wstring(argv[1]) == L"--child-crash") {
        return run_child_crash(std::filesystem::path(argv[2]));
    }

    Registry r;
    test_initialize_creates_directory(r);
    test_double_install_is_rejected(r);
    test_crash_leaves_a_report(r);
    return r.finish("crash_reporter_tests");
}
