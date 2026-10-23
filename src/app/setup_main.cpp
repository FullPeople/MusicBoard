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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/failure.h"
#include "install/install_transaction.h"
#include "core/path_utf8.h"
#include "install/product_registration.h"
#include "install/transaction_lock.h"
#include "distribution/SetupProgress.h"

using namespace musicboard;
using namespace musicboard::app;   // report_line_fmt / report / initialize_output

namespace {

constexpr wchar_t kExecutableName[] = L"MusicBoard.exe";
constexpr wchar_t kSetupName[] = L"MusicBoardSetup.exe";

struct SetupOptions {
    std::wstring registry_subkey;
    std::filesystem::path shortcut, desktop;
    std::optional<bool> desktop_enabled;
    std::wstring progress_channel;
    std::string manifest_sha256;
};

std::wstring quote_argument(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(slashes * (character == L'\"' ? 2 : 1), L'\\');
        if (character == L'\"') result += L'\\';
        result += character;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}

void apply_setup_options(ProductRegistrationOptions& registration, const SetupOptions& setup) {
    registration.uninstall_registry_subkey = setup.registry_subkey;
    registration.start_menu_shortcut = setup.shortcut;
    registration.desktop_shortcut = setup.desktop;
    registration.desktop_shortcut_enabled = setup.desktop_enabled.value_or(
        registered_desktop_shortcut_enabled(setup.registry_subkey, true));
}

std::wstring forwarded_options(const SetupOptions& setup) {
    std::wstring result;
    if (!setup.registry_subkey.empty()) result += L" --registry-subkey " + quote_argument(setup.registry_subkey);
    if (!setup.shortcut.empty()) result += L" --shortcut-path " + quote_argument(setup.shortcut.wstring());
    if (!setup.desktop.empty()) result += L" --desktop-shortcut-path " + quote_argument(setup.desktop.wstring());
    if (setup.desktop_enabled) result += *setup.desktop_enabled ? L" --desktop-shortcut" : L" --no-desktop-shortcut";
    if (!setup.progress_channel.empty()) result += L" --progress-channel " + quote_argument(setup.progress_channel);
    if (!setup.manifest_sha256.empty()) result += L" --manifest-sha256 " + quote_argument(std::wstring(setup.manifest_sha256.begin(), setup.manifest_sha256.end()));
    result += language() == Language::english ? L" --lang en" : L" --lang cn";
    return result;
}

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
bool wait_for_process(DWORD pid, DWORD timeout_ms) {
    const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        return ::GetLastError() == ERROR_INVALID_PARAMETER;
    }
    const DWORD result = ::WaitForSingleObject(process, timeout_ms);
    ::CloseHandle(process);
    return result == WAIT_OBJECT_0;
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
               bool quiet, bool launch, const SetupOptions& setup,
               fulltools::SetupProgressChannel& progress) {
    // ---- 1. 校验 ----
    VerifyOptions verify;
    verify.package_root = package_root;
    verify.expected_manifest_sha256 = setup.manifest_sha256;
    verify.progress = [&progress](std::uint64_t done, std::uint64_t total) {
        progress.write(fulltools::SetupStep::verify, done, total);
    };
    VerifiedPackage package;
    Failure failure;
    SetupTransactionLock transaction;
    if (!transaction.acquire(install_root, failure)) {
        report_failure(tr("暂时无法开始安装。", "Installation cannot start yet."), failure, quiet);
        return ERROR_BUSY;
    }
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
    options.progress = [&progress](std::uint64_t done, std::uint64_t total) {
        progress.write(fulltools::SetupStep::copy, done, total);
    };
    progress.write(fulltools::SetupStep::copy, 0, package.total_bytes);
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
    apply_setup_options(registration, setup);
    registration.install_root = install_root;
    registration.uninstaller_path = install_root / L"current" / kSetupName;
    registration.executable_path = result.installed_executable;
    registration.product_version = package.version;
    registration.executable_relative = std::wstring(L"current\\") + kExecutableName;
    // 估算大小给"设置 → 应用"显示（KB）
    registration.estimated_size_kib =
        static_cast<std::uint32_t>(package.total_bytes / 1024u);
    progress.write(fulltools::SetupStep::registration, 0, 1);

    // 卸载器要在安装目录里也有一份（就是本程序）
    {
        std::error_code ec;
        const bool same_file = std::filesystem::equivalent(executable_path(), registration.uninstaller_path, ec);
        if (!same_file) {
            ec.clear();
            std::filesystem::copy_file(executable_path(), registration.uninstaller_path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            // 没有卸载器还能用主程序，但"设置 → 应用"里的卸载按钮会点不动。
            // 说清楚，不静默。
            failure.set(fail_code::file_open_failed, "无法放置卸载程序: " + ec.message());
            report_failure(tr("安装尚未完成。", "Installation has not finished."), failure, quiet);
            return 4;
        }
    }

    if (!apply_product_registration(registration, failure)) {
        // 注册失败不回滚安装：程序已经装好了，能用；只是系统里没有卸载入口。
        // 明确告诉用户，并给出补救办法。
        report_failure(
            tr("程序已装好，但**没能写进系统的应用列表**（开始菜单与卸载入口可能缺失）。", "Installed, but could not register the app in Windows (Start menu and uninstall entries may be missing)."),
            failure, quiet);
        return 4;
    }
    report(tr("已注册卸载入口与开始菜单快捷方式。", "Uninstall entry and Start menu shortcut registered."));
    progress.write(fulltools::SetupStep::registration, 1, 1);

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
int do_uninstall(const std::filesystem::path& install_root, bool quiet, const SetupOptions& setup) {
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
                std::wstring command = quote_argument(temporary.wstring()) + L" --uninstall" +
                                       (quiet ? L" --quiet" : L"") +
                                       L" --install-root " + quote_argument(install_root.wstring()) +
                                       forwarded_options(setup) +
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
    SetupTransactionLock transaction;
    if (!transaction.acquire(install_root, failure)) {
        report_failure(tr("暂时无法开始卸载。", "Uninstall cannot start yet."), failure, quiet);
        return ERROR_BUSY;
    }
    ProductRegistrationOptions registration;
    apply_setup_options(registration, setup);
    registration.install_root = install_root;
    registration.uninstaller_path = current / kSetupName;
    registration.executable_path = current / kExecutableName;

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
    if (!self.empty() && !temp_root.empty() && inside(temp_root, self) &&
        self.filename().wstring().starts_with(L"MusicBoard-Uninstall-")) {
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
    SetupOptions setup;
    std::string argument_error;

    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        const auto next = [&]() -> std::wstring {
            if (i + 1 >= argc || std::wstring_view(argv[i + 1]).starts_with(L"--")) {
                argument_error = "Missing value: " + path_to_utf8(std::filesystem::path(argument));
                return {};
            }
            const std::wstring value = argv[++i];
            if (value.empty()) argument_error = "Empty value: " + path_to_utf8(std::filesystem::path(argument));
            return value;
        };
        if (argument == L"--uninstall") {
            uninstall = true;
        } else if (argument == L"--lang") {
            const std::wstring locale = next();
            if (locale != L"cn" && locale != L"en") { report("Unsupported --lang (use cn or en)"); return 1; }
            set_language(locale == L"en" ? Language::english : Language::chinese);
        } else if (argument == L"--quiet") {
            quiet = true;
        } else if (argument == L"--no-launch") {
            launch = false;
        } else if (argument == L"--package-root") {
            package_root = next();
        } else if (argument == L"--install-root") {
            install_root = next();
        } else if (argument == L"--registry-subkey") {
            setup.registry_subkey = next();
        } else if (argument == L"--shortcut-path") {
            setup.shortcut = next();
        } else if (argument == L"--desktop-shortcut-path") {
            setup.desktop = next();
        } else if (argument == L"--desktop-shortcut") {
            setup.desktop_enabled = true;
        } else if (argument == L"--no-desktop-shortcut") {
            setup.desktop_enabled = false;
        } else if (argument == L"--progress-channel") {
            setup.progress_channel = next();
        } else if (argument == L"--manifest-sha256") {
            const auto value = next();
            if (value.size() != 64 || value.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) argument_error = "Invalid manifest SHA-256";
            else for (wchar_t character : value) setup.manifest_sha256 += static_cast<char>(character);
        } else if (argument == L"--wait-pid") {
            const auto value = next();
            if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) argument_error = "Invalid --wait-pid";
            else {
                try { wait_pid = std::stoul(value); if (!wait_pid) argument_error = "Invalid --wait-pid"; }
                catch (...) { argument_error = "Invalid --wait-pid"; }
            }
        } else if (argument == L"--help" || argument == L"-h") {
            report_line_fmt(
                tr("音乐板安装程序\n  音乐板安装.exe [--quiet] [--no-launch] [--package-root <目录>]\n  音乐板安装.exe --uninstall [--install-root <目录>] [--quiet]\n", "MusicBoard Setup\n  MusicBoardSetup.exe [--quiet] [--no-launch] [--package-root <folder>] [--lang cn|en]\n  MusicBoardSetup.exe --uninstall [--install-root <folder>] [--quiet]\n"));
            return 0;
        } else {
            argument_error = "Unknown argument: " + path_to_utf8(std::filesystem::path(argument));
        }
        if (!argument_error.empty()) break;
    }

    const bool isolated = !setup.registry_subkey.empty() || !setup.shortcut.empty() || !setup.desktop.empty();
    if (isolated && (setup.registry_subkey.empty() || setup.shortcut.empty() || setup.desktop.empty() || install_root.empty() ||
                     _wcsicmp(setup.registry_subkey.c_str(), L"MusicBoard") == 0)) argument_error = "Isolated registration requires a private subkey, install root and both shortcut paths";
    if (!setup.registry_subkey.empty() && (setup.registry_subkey.size() > 128 || setup.registry_subkey.find_first_of(L"\\/\r\n") != std::wstring::npos)) argument_error = "Invalid registry subkey";
    for (const auto& path : {setup.shortcut, setup.desktop}) {
        if (!path.empty() && (!path.is_absolute() || path.extension() != L".lnk")) argument_error = "Shortcut path must be an absolute .lnk file";
    }
    if (!argument_error.empty()) { report(argument_error); return 1; }

    if (install_root.empty()) {
        const auto self = executable_path();
        install_root = uninstall && self.parent_path().filename() == L"current" ? self.parent_path().parent_path() : default_install_root();
        if (install_root.empty()) {
            report(tr("取不到 %LOCALAPPDATA%，无法确定安装位置。", "LOCALAPPDATA is unavailable; cannot determine the installation folder."));
            return 1;
        }
    }
    install_root = install_root.lexically_normal();
    if (!install_root.is_absolute() || install_root == install_root.root_path()) { report("Invalid installation root"); return 1; }
    fulltools::SetupProgressChannel progress;
    if (!setup.progress_channel.empty() && !progress.open(setup.progress_channel)) { report("Unable to open installation progress channel"); return 1; }

    if (uninstall) {
        if (wait_pid != 0) {
            report(tr("等待原进程退出…", "Waiting for the previous process to exit…"));
            if (!wait_for_process(wait_pid, 15000)) { report("The previous uninstaller did not exit; retry after closing it"); return 4; }
        }
        return do_uninstall(install_root, quiet, setup);
    }

    if (package_root.empty()) {
        package_root = executable_directory();
        if (package_root.empty()) {
            report(tr("取不到本程序所在目录，无法找到安装包。", "Cannot locate the executable folder or installation package."));
            return 1;
        }
    }
    return do_install(package_root, install_root, quiet, launch && !quiet, setup, progress);
}
