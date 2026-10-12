#include "core/localization.h"
// 音乐板安装/卸载程序
//
// 用法：
//   音乐板安装.exe                    装到 %LOCALAPPDATA%\Programs\MusicBoard，装完启动
//   音乐板安装.exe --quiet            同上，但不启动、不弹窗（打包冒烟测试用）
//   音乐板安装.exe --no-launch        装完不启动
//   音乐板安装.exe --package-root <目录>   指定包目录（默认：本程序所在目录）
//   音乐板安装.exe --uninstall        卸载
//   音乐板安装.exe --uninstall --install-root <目录>
//
// 为什么安装器自己也在包里：卸载时它和被装的版本天然一致。如果卸载器指向包外的
// 东西（比如"下载的那个安装包"），用户升级之后就会出现"旧卸载器卸新版本"。
//
// 卸载的自我删除：Windows 不允许删掉正在运行的 exe。所以 --uninstall 时如果自己
// 就在安装目录里，会先把**自己复制到 %TEMP%** 再从那跑一次，原进程退出，
// 临时副本负责把安装目录整个删掉（包括原来那个 exe），最后删掉自己。
// （DELAY_UNTIL_REBOOT 那条路要写 HKLM，需要管理员 —— 与"双击即装"冲突。）

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>   // CoInitializeEx / OleInitialize

#include <cstdio>

#include "app/app_output.h"
#include <filesystem>
#include <string>
#include <vector>

#include "core/failure.h"
#include "install/install_transaction.h"
#include "core/path_utf8.h"
#include "install/product_registration.h"

using namespace musicboard;
using namespace musicboard::app;   // report_line_fmt / report / initialize_output

namespace {

constexpr wchar_t kExecutableName[] = L"MusicBoard.exe";
constexpr wchar_t kSetupName[] = L"MusicBoardSetup.exe";

void report(const std::string& text) {
    report_line_fmt("%s\n", text.c_str());
    std::fflush(stdout);
}

void report_failure(const std::string& title, const Failure& failure, bool quiet) {
    const std::string text = title + "\n\n" + failure.to_string();
    report(text);
    if (quiet) {
        return;
    }
    // 双击启动时没有控制台，stdout 看不见 —— 失败必须弹出来，
    // 否则用户看到的是"双击了，什么都没发生"。
    const std::wstring wide = [&text] {
        std::wstring out;
        Failure ignored;
        static_cast<void>(utf8_to_wide(text, out, ignored));
        return out;
    }();
    ::MessageBoxW(nullptr, wide.c_str(), tr(L"音乐板安装", L"MusicBoard Setup"), MB_OK | MB_ICONERROR);
}

[[nodiscard]] std::filesystem::path executable_directory() {
    wchar_t buffer[32768]{};
    const DWORD written =
        ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
}

[[nodiscard]] std::filesystem::path executable_path() {
    wchar_t buffer[32768]{};
    const DWORD written =
        ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer);
}

// path 是否在 install_root 之内（卸载时的自我复制要用它判断"我是不是
// 就跑在安装目录里")。逐组件比较，避免 "C:\abc" 被 "C:\ab" 前缀误判。
[[nodiscard]] bool inside(const std::filesystem::path& root,
                          const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path target = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return false;
    }
    auto base_it = base.begin();
    auto target_it = target.begin();
    for (; base_it != base.end(); ++base_it, ++target_it) {
        if (target_it == target.end() || *base_it != *target_it) {
            return false;
        }
    }
    return target_it != target.end();
}

// 等另一个进程退出。卸载时用来等原来的自己退出 —— 不等的话
// 安装目录里那个 exe 还锁着，删不掉。
void wait_for_process(DWORD pid, DWORD timeout_ms) {
    const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        return;   // 已经退出了
    }
    ::WaitForSingleObject(process, timeout_ms);
    ::CloseHandle(process);
}

// 装完之后启动主程序。失败不算安装失败 —— 程序已经装好了，
// 只是没自动打开，用户自己点开始菜单就行。
void launch_installed(const std::filesystem::path& executable, const std::filesystem::path& cwd) {
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         cwd.c_str(), &startup, &info) != FALSE) {
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
    }
}

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------
int do_install(const std::filesystem::path& package_root, const std::filesystem::path& install_root,
               bool quiet, bool launch) {
    // ---- 1. 校验 ----
    VerifyOptions verify;
    verify.package_root = package_root;
    VerifiedPackage package;
    Failure failure;
    if (!verify_install_package(verify, package, failure)) {
        report_failure(tr("安装包校验失败。包可能不完整或被改动过。", "Package verification failed. The package may be incomplete or modified."), failure, quiet);
        return 2;
    }
    report(tr("安装包校验通过: 音乐板 ", "Package verified: MusicBoard ") + package.version + "（" + package.architecture + "，" +
           std::to_string(package.entries.size()) + tr(" 个文件）", " files)"));

    // ---- 2. 安装 ----
    InstallOptions options;
    options.package = package;
    options.install_root = install_root;
    InstallResult result;
    if (!install_product(options, result, failure)) {
        report_failure(tr("安装失败。", "Installation failed."), failure, quiet);
        return 3;
    }
    if (result.recovered_interrupted_transaction) {
        report(tr("上一次安装被中断，已经恢复。", "The interrupted installation has been recovered."));
    }
    report(result.upgraded ? (tr("已从 ", "Upgraded from ") + result.previous_version + tr(" 升级到 ", " to ") + result.installed_version)
                           : (tr("已安装 ", "Installed ") + result.installed_version));

    // ---- 3. 注册 ----
    ProductRegistrationOptions registration;
    registration.install_root = install_root;
    registration.uninstaller_path = install_root / L"current" / kSetupName;
    registration.executable_path = result.installed_executable;
    registration.product_version = package.version;
    registration.executable_relative = std::wstring(L"current\\") + kExecutableName;
    // 估算大小给"设置 → 应用"显示（KB）
    registration.estimated_size_kib =
        static_cast<std::uint32_t>(package.total_bytes / 1024u);

    // 卸载器要在安装目录里也有一份（就是本程序）
    {
        std::error_code ec;
        std::filesystem::copy_file(executable_path(), registration.uninstaller_path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            // 没有卸载器还能用主程序，但"设置 → 应用"里的卸载按钮会点不动。
            // 说清楚，不静默。
            report(tr("警告: 没能把安装程序复制到安装目录（卸载入口可能不可用）: ", "Warning: could not copy setup into the installation folder (uninstall may be unavailable): ") + ec.message());
        }
    }

    if (!apply_product_registration(registration, failure)) {
        // 注册失败不回滚安装：程序已经装好了，能用；只是系统里没有卸载入口。
        // 明确告诉用户，并给出补救办法。
        report_failure(
            tr("程序已装好，但**没能写进系统的应用列表**（开始菜单与卸载入口可能缺失）。", "Installed, but could not register the app in Windows (Start menu and uninstall entries may be missing)."),
            failure, quiet);
        if (!quiet) {
            launch_installed(result.installed_executable, result.installed_executable.parent_path());
        }
        return 0;
    }
    report(tr("已注册卸载入口与开始菜单快捷方式。", "Uninstall entry and Start menu shortcut registered."));

    report("");
    report(tr("音乐板安装完成。", "MusicBoard installation complete."));
    report(tr("  程序: ", "  Program: ") + path_to_utf8(result.installed_executable));
    report(tr("  卸载: 设置 → 应用 → 音乐板，或运行 ", "  Uninstall: Settings > Apps > MusicBoard, or run ") +
           path_to_utf8(registration.uninstaller_path) + " --uninstall");
    report(tr("  曲库: 首次启动用 --library <目录> 指一次，之后就记住了。", "  Library: select a folder on first launch or use --library <folder>."));

    if (launch) {
        launch_installed(result.installed_executable, result.installed_executable.parent_path());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 卸载
// ---------------------------------------------------------------------------
int do_uninstall(const std::filesystem::path& install_root, bool quiet) {
    const std::filesystem::path self = executable_path();
    const std::filesystem::path current = install_root / L"current";

    // 如果自己就在安装目录里，先把**自己复制到 %TEMP%** 再从那跑 ——
    // 正在运行的 exe 删不掉，复制出去才能把安装目录整个删干净。
    std::error_code ec;
    if (!self.empty() && inside(install_root, self)) {
        wchar_t temp_buffer[32768]{};
        const DWORD written = ::GetEnvironmentVariableW(L"TEMP", temp_buffer,
                                                       static_cast<DWORD>(std::size(temp_buffer)));
        if (written > 0 && written < std::size(temp_buffer)) {
            const std::filesystem::path temporary =
                std::filesystem::path(temp_buffer) /
                (L"MusicBoard-Uninstall-" + std::to_wstring(::GetCurrentProcessId()) + L".exe");
            std::filesystem::copy_file(self, temporary,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                std::wstring command = L"\"" + temporary.wstring() + L"\" --uninstall --quiet" +
                                       L" --install-root \"" + install_root.wstring() + L"\"" +
                                       L" --wait-pid " + std::to_wstring(::GetCurrentProcessId());
                STARTUPINFOW startup{};
                startup.cb = sizeof(startup);
                PROCESS_INFORMATION info{};
                if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != FALSE) {
                    ::CloseHandle(info.hThread);
                    ::CloseHandle(info.hProcess);
                    report(tr("卸载程序已经在后台启动，本窗口可以关闭。", "Uninstaller started in the background; you can close this window."));
                    return 0;
                }
                report(tr("警告: 无法启动临时卸载程序，将在当前进程里继续。", "Warning: could not start the temporary uninstaller; continuing in this process."));
            }
        }
    }

    Failure failure;
    ProductRegistrationOptions registration;
    registration.install_root = install_root;
    registration.uninstaller_path = current / kSetupName;

    // ---- 先删安装目录，再删注册 ----
    //
    // 顺序是刻意的：删目录是可能失败的那一步（文件被占用）。
    // 如果先删注册，删目录失败之后用户就再也没有入口重试了 ——
    // 他会看到程序还在、但"设置 → 应用"里已经没有它。
    UninstallOptions options;
    options.install_root = install_root;
    UninstallResult result;
    if (!uninstall_product(options, result, failure)) {
        report_failure(tr("卸载失败（可能有文件正在被占用，请先退出音乐板再试）", "Uninstall failed (files may be in use; exit MusicBoard and retry)"), failure, quiet);
        return 4;
    }
    report(tr("已删除安装目录", "Installation folder removed") +
           (result.removed_version.empty() ? std::string{}
                                           : std::string(tr("（版本 ", " (version ") + result.removed_version + "）")));

    if (!remove_product_registration(registration, failure)) {
        report_failure(tr("安装目录已删除，但**没能清掉系统里的应用列表条目**", "Installation folder removed, but could not remove the Windows app registration"), failure, quiet);
        return 5;
    }
    report(tr("已清掉卸载入口与开始菜单快捷方式。", "Uninstall entry and Start menu shortcut removed."));

    // 临时副本删掉自己：用 cmd 延迟删除（正在运行的 exe 删不掉）。
    //
    // **只有 %TEMP% 里的那一份才删自己。** 判据一度写成"我不在安装目录里"，
    // 那会把"用户从解压出来的包里直接跑 --uninstall"也算进去 ——
    // 于是它把用户包里的安装程序删掉了。端到端冒烟测试抓到的（打包完之后
    // 包里的 MusicBoardSetup.exe 不见了）。
    // 正确的判据是"我是不是那份临时副本"，也就是"我在不在 %TEMP% 里"。
    std::filesystem::path temp_root;
    {
        wchar_t temp_buffer[32768]{};
        const DWORD written = ::GetEnvironmentVariableW(L"TEMP", temp_buffer,
                                                       static_cast<DWORD>(std::size(temp_buffer)));
        if (written > 0 && written < std::size(temp_buffer)) {
            temp_root = std::filesystem::path(temp_buffer);
        }
    }
    if (!self.empty() && !temp_root.empty() && inside(temp_root, self)) {
        std::wstring command = L"cmd.exe /c ping 127.0.0.1 -n 2 > nul & del /f /q \"" +
                               self.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION info{};
        if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &startup, &info) != FALSE) {
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
        }
    }

    report("");
    report(tr("音乐板已卸载。", "MusicBoard uninstalled."));
    report(tr("  你的曲库、设置与日志没有被删除（它们在别的地方）。", "  Your library, settings and logs were kept in their original locations."));
    if (!quiet) {
        const std::wstring message = tr(L"音乐板已卸载。\n\n你的曲库、设置与日志没有被删除。", L"MusicBoard uninstalled.\n\nYour library, settings and logs were kept.");
        ::MessageBoxW(nullptr, message.c_str(), tr(L"音乐板卸载", L"MusicBoard Uninstall"), MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 安装程序同样是 GUI 子系统：双击时不弹控制台。
    // 输出走 app_output（终端里有控制台就打在终端里，否则进日志文件）。
    const std::filesystem::path log_path = app::initialize_output();
    static_cast<void>(log_path);
    static_cast<void>(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    ::OleInitialize(nullptr);

    bool uninstall = false;
    bool quiet = false;
    bool launch = true;
    DWORD wait_pid = 0;
    std::filesystem::path package_root;
    std::filesystem::path install_root;

    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--uninstall") {
            uninstall = true;
        } else if (argument == L"--lang" && i + 1 < argc) {
            const std::wstring locale = argv[++i];
            if (locale != L"cn" && locale != L"en") { report("Unsupported --lang (use cn or en)"); return 1; }
            set_language(locale == L"en" ? Language::english : Language::chinese);
        } else if (argument == L"--quiet") {
            quiet = true;
        } else if (argument == L"--no-launch") {
            launch = false;
        } else if (argument == L"--package-root" && i + 1 < argc) {
            package_root = std::filesystem::path(argv[++i]);
        } else if (argument == L"--install-root" && i + 1 < argc) {
            install_root = std::filesystem::path(argv[++i]);
        } else if (argument == L"--wait-pid" && i + 1 < argc) {
            wait_pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (argument == L"--help" || argument == L"-h") {
            report_line_fmt(
                tr("音乐板安装程序\n  音乐板安装.exe [--quiet] [--no-launch] [--package-root <目录>]\n  音乐板安装.exe --uninstall [--install-root <目录>] [--quiet]\n", "MusicBoard Setup\n  MusicBoardSetup.exe [--quiet] [--no-launch] [--package-root <folder>] [--lang cn|en]\n  MusicBoardSetup.exe --uninstall [--install-root <folder>] [--quiet]\n"));
            return 0;
        }
    }

    if (install_root.empty()) {
        install_root = default_install_root();
        if (install_root.empty()) {
            report(tr("取不到 %LOCALAPPDATA%，无法确定安装位置。", "LOCALAPPDATA is unavailable; cannot determine the installation folder."));
            return 1;
        }
    }

    if (uninstall) {
        if (wait_pid != 0) {
            report(tr("等待原进程退出…", "Waiting for the previous process to exit…"));
            wait_for_process(wait_pid, 15000);
        }
        return do_uninstall(install_root, quiet);
    }

    if (package_root.empty()) {
        package_root = executable_directory();
        if (package_root.empty()) {
            report(tr("取不到本程序所在目录，无法找到安装包。", "Cannot locate the executable folder or installation package."));
            return 1;
        }
    }
    return do_install(package_root, install_root, quiet, launch && !quiet);
}
