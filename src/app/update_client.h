#pragma once
// MIT: FullPeople. Shared native client for the standalone signed updater.
#include <windows.h>
#include <string>
#include <string_view>
namespace fulltools_update_client {
[[nodiscard]] inline bool launch(std::wstring_view product, std::string_view version, bool background, std::string& error) {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) { error = "updater.module_path Win32=" + std::to_string(GetLastError()); return false; }
    path.resize(length); const auto separator = path.find_last_of(L"/\\");
    if (separator == std::wstring::npos) { error = "updater.module_path has no parent directory"; return false; }
    path.resize(separator + 1); path += L"FullToolsUpdater.exe";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        error = "updater.component_missing: install the complete distribution package"; return false;
    }
    if (product.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != std::wstring_view::npos ||
        version.empty() || version.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.+-") != std::string_view::npos) {
        error = "updater.invalid_product_or_version"; return false;
    }
    std::wstring wide_version; for (char c : version) wide_version += static_cast<wchar_t>(static_cast<unsigned char>(c));
    std::wstring command = L"\"" + path + L"\" --product " + std::wstring(product) + L" --version " + wide_version;
    if (background) command += L" --background";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        error = "updater.launch Win32=" + std::to_string(GetLastError()); return false;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
}
}
