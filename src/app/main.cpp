#include "core/localization.h"
// 音乐板：程序入口
//
// 这个文件只做四件事：修好输出与 COM、解析参数、把 Application 建起来、跑消息循环。
// 真正的接线在 application.cpp。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

#include <cstdio>

#include "app/app_output.h"
#include <filesystem>
#include <string>

#include "app/application.h"
#include "core/failure.h"
#include "core/com_apartment.h"
#include "core/crash_reporter.h"
#include "core/path_utf8.h"
#include "core/product_settings.h"
#include "core/single_instance.h"

using namespace musicboard;
using namespace musicboard::app;   // report_line_fmt / report / initialize_output

namespace {

void print_usage() {
    report_line_fmt(
        tr("音乐板\n  音乐板.exe [--library <曲库目录>]\n\n  --library <目录>   指定曲库根目录（BGM/SFX 在里面）。\n                     不给则用设置里记的那个；设置里也没有时会明确报错。\n  --settings-path    打印设置文件位置后退出（排查配置问题时用）\n  --selftest         交互自检后退出（走与真人操作相同的事件路径）\n  --selftest-display DPI 通知、物理点击与底栏开关回归后退出\n  --version          打印版本后退出\n", "MusicBoard\n  MusicBoard.exe [--library <folder>]\n\n  --library <folder>  Library root containing BGM/SFX. Uses saved path when omitted.\n  --settings-path    Print settings path and exit\n  --selftest         Run interaction checks\n  --selftest-display Run DPI and footer checks\n  --version          Print version and exit\n"));
}

}  // namespace

// 必须用 wmain：MSVC 的 char** argv 走活动代码页(936)，中文路径会被打乱。
namespace {

// UTF-8 → 宽字符，给 MessageBoxW 用。转换失败时退化成"逐字节加宽"——
// 只用于错误提示，宁可显示得难看，也不要一个空框。
std::wstring utf8_to_wstring(const std::string& text) {
    std::wstring wide;
    Failure ignored;
    if (!utf8_to_wide(text, wide, ignored)) {
        return std::wstring(text.begin(), text.end());
    }
    return wide;
}

// 找到某个进程里指定类名的顶层窗口。
// 第二实例要把第一实例的面板叫出来，就得先找到那个窗口 ——
// 用 FindWindow 试过，跨进程一直返回 0（同进程枚举却能拿到类名），
// 所以这里按 pid 枚举，别跟 FindWindow 较劲。
struct EnumContext final {
    DWORD pid = 0;
    const wchar_t* class_name = nullptr;
    HWND found = nullptr;
};

BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<EnumContext*>(param);
    DWORD window_pid = 0;
    ::GetWindowThreadProcessId(hwnd, &window_pid);
    if (window_pid != context->pid) {
        return TRUE;
    }
    wchar_t class_name[256]{};
    if (::GetClassNameW(hwnd, class_name, 256) == 0) {
        return TRUE;
    }
    if (std::wcscmp(class_name, context->class_name) == 0) {
        context->found = hwnd;
        return FALSE;   // 找到了就不必再枚举
    }
    return TRUE;
}

[[nodiscard]] HWND find_process_window(DWORD pid, const wchar_t* class_name) {
    EnumContext context;
    context.pid = pid;
    context.class_name = class_name;
    ::EnumWindows(&enum_windows_proc, reinterpret_cast<LPARAM>(&context));
    return context.found;
}

// 把已有实例叫到前面来。返回是否真的找到了它的窗口。
//
// 要轮询等：第一个实例可能还在扫曲库（几秒），窗口还没建出来。
// 不等的话"双击第二次没反应"就会变成一个偶发问题。
[[nodiscard]] bool surface_existing_instance(DWORD pid) {
    constexpr int kAttempts = 20;
    constexpr DWORD kWaitMs = 500;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        HWND panel = find_process_window(pid, L"MusicBoard.CompositionWindow");
        if (panel == nullptr) {
            // 还没有面板：可能只有悬浮球，也可能两者都还没建
            HWND orb = find_process_window(pid, L"MusicBoard.OrbWindow");
            if (orb != nullptr) {
                panel = orb;
            }
        }
        if (panel != nullptr) {
            // 先让"那个进程"有权抢前台，再请它自己把面板显出来：
            // 跨进程调用 SetForegroundWindow 会被系统挡住，让它自己调才行。
            static_cast<void>(::AllowSetForegroundWindow(pid));
            const unsigned int message = show_panel_message();
            if (message != 0) {
                static_cast<void>(::PostMessageW(panel, message, 0, 0));
            }
            return true;
        }
        ::Sleep(kWaitMs);
    }
    return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 不缓冲。Debug CRT 的断言失败会直接 abort()，而重定向时 stdout 是块缓冲的，
    // 崩溃前打印的每一行都会丢掉 —— 现场只剩一个退出码。踩过两次，写进这里当默认。
    static_cast<void>(std::setvbuf(stdout, nullptr, _IONBF, 0));
#if defined(_MSC_VER)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif

    // 全局快捷键、通知区域、Shell 快捷方式都要 COM/Shell 环境。
    // OLE 是为了将来的拖放（DoDragDrop）与 Shell 链接。
    ComApartment apartment;
    Failure apartment_failure;
    if (!apartment.initialize(COINIT_APARTMENTTHREADED, apartment_failure)) {
        report_line_fmt(tr("COM 初始化失败: %s\n", "COM initialization failed: %s\n"), apartment_failure.to_string().c_str());
        return 1;
    }

    // 输出去向：附到父控制台（从终端启动时）并同时开一份日志文件。
    // GUI 子系统下没有控制台时，日志文件是唯一的现场 —— 双击启动出问题时
    // 至少有一处可以查，而不是对着空气猜。
    const std::filesystem::path log_path = app::initialize_output();

    std::filesystem::path library;
    bool print_settings = false;
    bool print_version = false;
    bool self_test = false;
    bool test_display = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--library" && i + 1 < argc) {
            // 直接拿宽字符构造 path —— 走 UTF-8 再转回来是多余的，
            // 而且窄字符那一步会经过活动代码页（那是本项目最怕的一类错）。
            library = std::filesystem::path(argv[++i]);
        } else if (argument == L"--settings-path") {
            print_settings = true;
        } else if (argument == L"--version") {
            print_version = true;
        } else if (argument == L"--selftest") {
            self_test = true;
        } else if (argument == L"--selftest-display") {
            self_test = true;
            test_display = true;
        } else if (argument == L"--help" || argument == L"-h") {
            print_usage();
            return 0;
        }
    }

    if (print_settings) {
        report_line_fmt("%s\n", path_to_utf8(default_settings_path()).c_str());
        return 0;
    }
    if (print_version) {
        report_line_fmt(tr("音乐板 %s\n", "MusicBoard %s\n"), MUSICBOARD_PRODUCT_VERSION);
        return 0;
    }

    // ---- 单实例 ----
    //
    // 跑团的时候这套东西是常驻的，双击图标是"我要看面板"而不是"再起一套"。
    // 所以已经在跑时：把已有实例叫到前面，然后自己退出。
    // 自检不占这个名额 —— 否则"一边开着程序一边跑自检"会直接被挡掉。
    SingleInstanceGuard guard;
    if (!self_test) {
        SingleInstanceState instance_state = SingleInstanceState::acquired;
        Failure guard_failure;
        if (!guard.initialize(product_single_instance_guard_name, instance_state, guard_failure)) {
            // 守卫本身出问题不该让程序起不来：说清楚，然后照常启动。
            app::report(LogLevel::error, tr("单实例守卫不可用", "Single-instance guard unavailable"), guard_failure.to_string());
        } else if (instance_state == SingleInstanceState::already_running) {
            const DWORD existing = guard.existing_process_id();
            app::report_line_fmt(tr("已有实例在运行（pid %lu），把它叫到前面来。\n", "Already running (pid %lu); opening the existing panel.\n"),
                                 static_cast<unsigned long>(existing));
            const bool surfaced = surface_existing_instance(existing);
            if (!surfaced) {
                app::report_line_fmt(
                    tr("没找到已有实例的窗口；它可能还在启动或已经卡住。日志：%s\n", "Could not locate the existing window. It may be starting or unresponsive. Log: %s\n"),
                    path_to_utf8(log_path).c_str());
            }
            return 0;
        }
    }

    CrashReporter crash_reporter;
    Failure crash_failure;
    if (!crash_reporter.initialize(product_data_root() / L"crashes", crash_failure)) {
        app::report(LogLevel::error, tr("崩溃诊断初始化失败", "Crash diagnostics initialization failed"), crash_failure.to_string());
        return 1;
    }
    app::Application application;
    Failure failure;
    if (!application.initialize(library, failure)) {
        if (failure.code == "app.cancelled") {
            app::report(LogLevel::info, tr("启动已取消", "Startup cancelled"), failure.context);
            return 0;
        }
        app::report(LogLevel::error, tr("启动失败", "Startup failed"), failure.to_string());
        // 双击启动时用户看不到任何输出，所以**必须**把话说在一个看得见的地方。
        // 起不来就是起不来，不能让人对着空气猜。
        const std::wstring message =
            tr(L"音乐板启动失败：\n\n", L"MusicBoard could not start:\n\n") + utf8_to_wstring(failure.context) +
            tr(L"\n\n首次使用需要指定曲库目录，例如：\n  musicboard.exe --library \"D:\\你的曲库\"\n\n也可以直接编辑设置文件：\n  ", L"\n\nOn first use, select your library, for example:\n  musicboard.exe --library \"D:\\MyMusic\"\n\nOr edit the settings file:\n  ") +
            utf8_to_wstring(path_to_utf8(default_settings_path()));
        ::MessageBoxW(nullptr, message.c_str(), tr(L"音乐板", L"MusicBoard"), MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!application.create_windows(failure)) {
        app::report(LogLevel::error, tr("窗口创建失败", "Window creation failed"), failure.to_string());
        const std::wstring message = tr(L"音乐板窗口创建失败：\n\n", L"MusicBoard window creation failed:\n\n") +
                                     utf8_to_wstring(failure.context) +
                                     tr(L"\n\n日志： ", L"\n\nLog: ") + utf8_to_wstring(path_to_utf8(log_path));
        ::MessageBoxW(nullptr, message.c_str(), tr(L"音乐板", L"MusicBoard"), MB_OK | MB_ICONERROR);
        return 1;
    }
    if (self_test) {
        // 交互自检：走与真人操作完全相同的事件处理路径，逐条断言结果。
        return test_display ? application.self_test_display() : application.self_test();
    }
    return application.run();
}
