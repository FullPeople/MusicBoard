#include "install/product_registration.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>   // ComPtr

#include <string>
#include <vector>

#include "core/path_utf8.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace musicboard {
namespace {

constexpr wchar_t kUninstallKeyRoot[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";

// UTF-8 → 宽字符。
//
// **绝不要写 std::wstring(text.begin(), text.end())** —— 那会把每一个字节
// 单独扩成一个 wchar_t，中文变成一串乱码。这个坑真的踩过：注册表里的
// DisplayName 是"音乐板"，写进去的是 9 个 Latin-1 字符，
// "设置 → 应用"里显示成 `???￤?????`。
// 打包冒烟测试抓到的（它读回注册表值与期望的宽字符串比较）。
[[nodiscard]] std::wstring to_wide(std::string_view utf8) {
    std::wstring out;
    Failure ignored;
    if (!utf8_to_wide(utf8, out, ignored)) {
        return {};
    }
    return out;
}

[[nodiscard]] std::wstring uninstall_key_path(const std::wstring& subkey) {
    return std::wstring(kUninstallKeyRoot) + subkey;
}

// 注册表值的写入/读取/删除，统一走这几个小函数 ——
// 散落的 RegSetValueExW 很容易在某个分支漏掉一次 RegCloseKey。
class RegistryKey final {
public:
    ~RegistryKey() {
        if (key_ != nullptr) {
            ::RegCloseKey(key_);
        }
    }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    RegistryKey() = default;

    [[nodiscard]] bool create(const std::wstring& path, Failure& failure) {
        const LSTATUS status = ::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                                                KEY_WRITE | KEY_READ, nullptr, &key_, nullptr);
        if (status != ERROR_SUCCESS) {
            failure.set(fail_code::platform_api_failed,
                        "创建注册表键失败（" + std::to_string(status) + "）: " +
                            path_to_utf8(std::filesystem::path(path)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool open_read(const std::wstring& path, Failure& failure) {
        const LSTATUS status =
            ::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key_);
        if (status != ERROR_SUCCESS) {
            failure.set(fail_code::file_not_found,
                        "注册表键不存在（" + std::to_string(status) + "）: " +
                            path_to_utf8(std::filesystem::path(path)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool set_string(const wchar_t* name, const std::wstring& value,
                                  Failure& failure) {
        const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        const LSTATUS status = ::RegSetValueExW(key_, name, 0, REG_SZ,
                                               reinterpret_cast<const BYTE*>(value.c_str()),
                                               bytes);
        if (status != ERROR_SUCCESS) {
            failure.set(fail_code::platform_api_failed,
                        "写注册表值失败（" + std::to_string(status) + "）: " +
                            path_to_utf8(std::filesystem::path(name)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool set_dword(const wchar_t* name, DWORD value, Failure& failure) {
        const LSTATUS status = ::RegSetValueExW(key_, name, 0, REG_DWORD,
                                               reinterpret_cast<const BYTE*>(&value),
                                               sizeof(value));
        if (status != ERROR_SUCCESS) {
            failure.set(fail_code::platform_api_failed,
                        "写注册表值失败（" + std::to_string(status) + "）");
            return false;
        }
        return true;
    }

    [[nodiscard]] std::wstring read_string(const wchar_t* name) const {
        wchar_t buffer[4096]{};
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        const LSTATUS status = ::RegQueryValueExW(key_, name, nullptr, &type,
                                                 reinterpret_cast<BYTE*>(buffer), &size);
        if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
            return {};
        }
        return buffer;
    }

    [[nodiscard]] HKEY get() const noexcept { return key_; }

private:
    HKEY key_ = nullptr;
};

[[nodiscard]] bool delete_key_tree(const std::wstring& path, Failure& failure) {
    const LSTATUS status = ::RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        failure.set(fail_code::platform_api_failed,
                    "删除注册表键失败（" + std::to_string(status) + "）: " +
                        path_to_utf8(std::filesystem::path(path)));
        return false;
    }
    return true;
}

class RegistrationSnapshot final {
    struct Value { const wchar_t* name; DWORD type = 0; bool exists = false; std::vector<BYTE> data; };
    std::wstring path_;
    bool existed_ = false;
    std::vector<Value> values_;
public:
    bool capture(const std::wstring& path, Failure& failure) {
        path_ = path;
        HKEY key = nullptr;
        const LSTATUS status = ::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key);
        if (status == ERROR_FILE_NOT_FOUND) return true;
        if (status != ERROR_SUCCESS) {
            failure.set(fail_code::platform_api_failed, "无法保留原安装登记（" + std::to_string(status) + "）");
            return false;
        }
        existed_ = true;
        bool success = true;
        for (const wchar_t* name : {L"DisplayName", L"DisplayVersion", L"Publisher", L"InstallLocation",
                L"UninstallString", L"QuietUninstallString", L"DesktopShortcutEnabled", L"DisplayIcon",
                L"NoModify", L"NoRepair", L"EstimatedSize"}) {
            Value value{name, 0, false, {}};
            DWORD bytes = 0;
            LSTATUS read = ::RegQueryValueExW(key, name, nullptr, &value.type, nullptr, &bytes);
            if (read == ERROR_SUCCESS) {
                value.exists = true;
                value.data.resize(bytes);
                read = ::RegQueryValueExW(key, name, nullptr, &value.type, value.data.data(), &bytes);
            }
            if (read != ERROR_SUCCESS && read != ERROR_FILE_NOT_FOUND) { success = false; break; }
            values_.push_back(std::move(value));
        }
        ::RegCloseKey(key);
        if (!success) failure.set(fail_code::platform_api_failed, "无法完整保留原安装登记");
        return success;
    }
    void restore(Failure& failure) const {
        if (!existed_) {
            Failure cleanup;
            if (!delete_key_tree(path_, cleanup)) failure.context += "; " + cleanup.context;
            return;
        }
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
            failure.context += "; 无法恢复原安装登记，请重试安装";
            return;
        }
        bool success = true;
        for (const auto& value : values_) {
            const LSTATUS written = value.exists ? ::RegSetValueExW(key, value.name, 0, value.type,
                    value.data.data(), static_cast<DWORD>(value.data.size())) : ::RegDeleteValueW(key, value.name);
            if (written != ERROR_SUCCESS && written != ERROR_FILE_NOT_FOUND) success = false;
        }
        ::RegCloseKey(key);
        if (!success) failure.context += "; 原安装登记恢复不完整，请重试安装";
    }
};

// 开始菜单快捷方式。用 IShellLink 而不是自己写 .lnk 的二进制格式 ——
// 那个格式没有公开文档，手写必错。
[[nodiscard]] bool create_shortcut(const std::filesystem::path& shortcut_path,
                                  const std::filesystem::path& target,
                                  const std::filesystem::path& working_directory,
                                  const std::wstring& description, Failure& failure) {
    std::error_code ec;
    std::filesystem::create_directories(shortcut_path.parent_path(), ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "创建快捷方式所在目录失败 " + path_to_utf8(shortcut_path.parent_path()) +
                        ": " + ec.message());
        return false;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IShellLinkW> link;
    HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(link.GetAddressOf()));
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "创建 ShellLink 失败");
        return false;
    }
    if (FAILED(link->SetPath(target.c_str())) ||
        FAILED(link->SetWorkingDirectory(working_directory.c_str())) ||
        FAILED(link->SetDescription(description.c_str())) ||
        FAILED(link->SetIconLocation(target.c_str(), 0))) {
        failure.set(fail_code::platform_api_failed, "设置快捷方式属性失败");
        return false;
    }

    ComPtr<IPersistFile> file;
    hr = link.As(&file);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "取 IPersistFile 失败");
        return false;
    }
    hr = file->Save(shortcut_path.c_str(), TRUE);
    if (FAILED(hr)) {
        failure.set(fail_code::file_open_failed,
                    "写快捷方式失败 " + path_to_utf8(shortcut_path) + "（hr=" +
                        std::to_string(static_cast<unsigned long>(hr)) + "）");
        return false;
    }
    return true;
}

[[nodiscard]] std::wstring quote_argument(const std::wstring& value) {
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

[[nodiscard]] bool shortcut_targets(const std::filesystem::path& shortcut,
                                     const std::filesystem::path& expected) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(link.GetAddressOf()))) ||
        FAILED(link.As(&file)) || FAILED(file->Load(shortcut.c_str(), STGM_READ))) return false;
    wchar_t target[32768]{};
    if (FAILED(link->GetPath(target, static_cast<int>(std::size(target)), nullptr, SLGP_RAWPATH))) return false;
    std::error_code error;
    auto actual = std::filesystem::weakly_canonical(target, error);
    if (error) actual = std::filesystem::path(target).lexically_normal();
    auto wanted = std::filesystem::weakly_canonical(expected, error);
    if (error) wanted = expected.lexically_normal();
    return _wcsicmp(actual.c_str(), wanted.c_str()) == 0;
}

[[nodiscard]] bool update_owned_shortcut(const std::filesystem::path& shortcut,
                                         const std::filesystem::path& target,
                                         const std::filesystem::path& working_directory,
                                         const std::wstring& description, bool enabled,
                                         Failure& failure) {
    if (shortcut.empty()) {
        if (!enabled) return true;
        failure.set(fail_code::platform_api_failed, "取不到快捷方式目录");
        return false;
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(shortcut, error);
    if (error) {
        failure.set(fail_code::file_open_failed, "无法读取快捷方式: " + path_to_utf8(shortcut));
        return false;
    }
    if (exists && !shortcut_targets(shortcut, target)) {
        if (!enabled) return true;
        failure.set(fail_code::file_open_failed,
                    "同名快捷方式属于其他程序，已保留: " + path_to_utf8(shortcut));
        return false;
    }
    if (enabled) return create_shortcut(shortcut, target, working_directory, description, failure);
    if (exists && !std::filesystem::remove(shortcut, error)) {
        failure.set(fail_code::file_open_failed, "删除快捷方式失败: " + path_to_utf8(shortcut));
        return false;
    }
    return true;
}

[[nodiscard]] std::wstring uninstall_command(const ProductRegistrationOptions& options,
                                             bool quiet) {
    std::wstring command = quote_argument(options.uninstaller_path.wstring()) +
        L" --uninstall --install-root " + quote_argument(options.install_root.wstring());
    if (!options.uninstall_registry_subkey.empty()) command += L" --registry-subkey " + quote_argument(options.uninstall_registry_subkey);
    if (!options.start_menu_shortcut.empty()) command += L" --shortcut-path " + quote_argument(options.start_menu_shortcut.wstring());
    if (!options.desktop_shortcut.empty()) command += L" --desktop-shortcut-path " + quote_argument(options.desktop_shortcut.wstring());
    if (quiet) command += L" --quiet";
    return command;
}

}  // namespace

std::filesystem::path default_start_menu_shortcut() {
    PWSTR folder = nullptr;
    // FOLDERID_Programs 就是 %APPDATA%\Microsoft\Windows\Start Menu\Programs
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &folder)) ||
        folder == nullptr) {
        return {};
    }
    std::filesystem::path path(folder);
    ::CoTaskMemFree(folder);
    return path / L"音乐板.lnk";
}

std::filesystem::path default_desktop_shortcut() {
    PWSTR folder = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &folder)) || !folder) return {};
    const auto result = std::filesystem::path(folder) / L"音乐板.lnk";
    ::CoTaskMemFree(folder);
    return result;
}

bool registered_desktop_shortcut_enabled(const std::wstring& subkey, bool fallback) {
    RegistryKey key;
    Failure ignored;
    if (!key.open_read(uninstall_key_path(subkey.empty() ? default_uninstall_registry_subkey() : subkey), ignored)) return fallback;
    DWORD value = 0, type = 0, bytes = sizeof(value);
    if (::RegQueryValueExW(key.get(), L"DesktopShortcutEnabled", nullptr, &type,
                           reinterpret_cast<BYTE*>(&value), &bytes) != ERROR_SUCCESS ||
        type != REG_DWORD || bytes != sizeof(value)) return fallback;
    return value != 0;
}

std::wstring default_uninstall_registry_subkey() {
    return L"MusicBoard";
}

bool apply_product_registration(const ProductRegistrationOptions& options, Failure& failure) {
    if (options.install_root.empty() || options.uninstaller_path.empty()) {
        failure.set(fail_code::invalid_argument,
                    "注册产品需要安装目录与卸载器路径");
        return false;
    }

    const std::wstring subkey =
        options.uninstall_registry_subkey.empty() ? default_uninstall_registry_subkey()
                                                 : options.uninstall_registry_subkey;
    const std::wstring key_path = uninstall_key_path(subkey);
    const std::filesystem::path shortcut = options.start_menu_shortcut.empty()
                                               ? default_start_menu_shortcut()
                                               : options.start_menu_shortcut;
    if (shortcut.empty()) {
        failure.set(fail_code::platform_api_failed, "取不到开始菜单目录");
        return false;
    }

    const auto target = options.executable_path.empty() ? options.uninstaller_path : options.executable_path;
    const auto desktop = options.desktop_shortcut.empty() ? default_desktop_shortcut() : options.desktop_shortcut;
    for (const auto& link : {shortcut, options.desktop_shortcut_enabled ? desktop : std::filesystem::path{}}) {
        std::error_code error;
        if (!link.empty() && std::filesystem::exists(link, error) && !shortcut_targets(link, target)) {
            failure.set(fail_code::file_open_failed, "同名快捷方式属于其他程序，已保留: " + path_to_utf8(link));
            return false;
        }
    }

    // ---- 注册表 ----
    RegistrationSnapshot snapshot;
    if (!snapshot.capture(key_path, failure)) return false;
    RegistryKey key;
    if (!key.create(key_path, failure)) {
        return false;
    }
    const std::wstring install_location = options.install_root.wstring();
    const std::wstring uninstall_string = uninstall_command(options, false);
    const std::wstring icon = options.executable_path.empty()
                                  ? options.uninstaller_path.wstring()
                                  : options.executable_path.wstring();

    const bool registry_ok =
        key.set_string(L"DisplayName", to_wide(options.product_name), failure) &&
        key.set_string(L"DisplayVersion", to_wide(options.product_version), failure) &&
        key.set_string(L"Publisher", to_wide(options.publisher), failure) &&
        key.set_string(L"InstallLocation", install_location, failure) &&
        key.set_string(L"UninstallString", uninstall_string, failure) &&
        // QuietUninstallString：控制面板里"卸载"不带交互时用它
        key.set_string(L"QuietUninstallString", uninstall_command(options, true), failure) &&
        key.set_dword(L"DesktopShortcutEnabled", options.desktop_shortcut_enabled ? 1u : 0u, failure) &&
        key.set_string(L"DisplayIcon", icon, failure) &&
        // 这个产品没有"修改"与"修复"这两种操作，明确关掉，
        // 免得控制面板给出两个点了没反应的按钮
        key.set_dword(L"NoModify", 1, failure) && key.set_dword(L"NoRepair", 1, failure) &&
        key.set_dword(L"EstimatedSize", options.estimated_size_kib, failure);
    if (!registry_ok) {
        // 回滚：不留半个注册。用户看到"安装失败"但注册表里少一半，
        // 下一次安装会以为已经装过。
        snapshot.restore(failure);
        return false;
    }

    // ---- 开始菜单快捷方式 ----
    const bool shortcut_ok = update_owned_shortcut(
        shortcut,
        options.executable_path.empty() ? options.uninstaller_path : options.executable_path,
        options.install_root, to_wide(options.product_name), true, failure) &&
        update_owned_shortcut(options.desktop_shortcut.empty() ? default_desktop_shortcut() : options.desktop_shortcut,
                             options.executable_path.empty() ? options.uninstaller_path : options.executable_path,
                             options.install_root, to_wide(options.product_name), options.desktop_shortcut_enabled, failure);
    if (!shortcut_ok) {
        // 同样回滚：注册表删掉，免得出现"注册表里有、开始菜单里没有"的中间态
        snapshot.restore(failure);
        return false;
    }
    return true;
}

bool remove_product_registration(const ProductRegistrationOptions& options, Failure& failure) {
    const std::wstring subkey =
        options.uninstall_registry_subkey.empty() ? default_uninstall_registry_subkey()
                                                 : options.uninstall_registry_subkey;
    RegistryKey existing;
    Failure absent;
    if (existing.open_read(uninstall_key_path(subkey), absent)) {
        const auto location = existing.read_string(L"InstallLocation");
        if (!location.empty() && _wcsicmp(std::filesystem::path(location).lexically_normal().c_str(),
                                          options.install_root.lexically_normal().c_str()) != 0) {
            failure.set(fail_code::invalid_argument, "系统卸载登记属于另一安装目录，已保留");
            return false;
        }
    }
    const std::filesystem::path shortcut = options.start_menu_shortcut.empty()
                                               ? default_start_menu_shortcut()
                                               : options.start_menu_shortcut;
    const auto target = options.executable_path.empty() ? options.install_root / L"current" / L"MusicBoard.exe" : options.executable_path;
    if (!update_owned_shortcut(shortcut, target, options.install_root, L"", false, failure) ||
        !update_owned_shortcut(options.desktop_shortcut.empty() ? default_desktop_shortcut() : options.desktop_shortcut,
                              target, options.install_root, L"", false, failure)) return false;
    return delete_key_tree(uninstall_key_path(subkey), failure);
}

bool product_registration_matches(const ProductRegistrationOptions& options, bool& matches,
                                 Failure& failure) {
    matches = false;
    const std::wstring subkey =
        options.uninstall_registry_subkey.empty() ? default_uninstall_registry_subkey()
                                                 : options.uninstall_registry_subkey;

    RegistryKey key;
    if (!key.open_read(uninstall_key_path(subkey), failure)) {
        // 没注册过不算错误，只是"不匹配"
        failure.clear();
        return true;
    }

    const std::wstring version = key.read_string(L"DisplayVersion");
    const std::wstring location = key.read_string(L"InstallLocation");
    const std::wstring uninstall = key.read_string(L"UninstallString");
    const std::wstring display = key.read_string(L"DisplayName");

    // 期望值也要经**同一个** UTF-8 → 宽字符转换。
    // 这里一度两边都用 "逐字节加宽"，于是两边一样地错、断言照样通过 ——
    // "把实现和自己比"是一种看不出问题的假验证。
    const std::wstring expected_version = to_wide(options.product_version);
    const bool version_ok = expected_version.empty() || version == expected_version;
    const bool location_ok =
        options.install_root.empty() || location == options.install_root.wstring();
    const bool uninstall_ok = options.uninstaller_path.empty() ||
                              uninstall.find(options.uninstaller_path.wstring()) !=
                                  std::wstring::npos;
    const bool display_ok = display == to_wide(options.product_name);

    std::error_code ec;
    const std::filesystem::path shortcut = options.start_menu_shortcut.empty()
                                               ? default_start_menu_shortcut()
                                               : options.start_menu_shortcut;
    const bool shortcut_ok = shortcut.empty() || std::filesystem::is_regular_file(shortcut, ec);

    matches = version_ok && location_ok && uninstall_ok && display_ok && shortcut_ok;
    return true;
}

std::string registered_version(const std::wstring& subkey) {
    RegistryKey key;
    Failure failure;
    const std::wstring name = subkey.empty() ? default_uninstall_registry_subkey() : subkey;
    if (!key.open_read(uninstall_key_path(name), failure)) {
        return {};
    }
    return path_to_utf8(std::filesystem::path(key.read_string(L"DisplayVersion")));
}

}  // namespace musicboard
