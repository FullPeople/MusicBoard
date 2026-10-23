#pragma once

#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include "core/failure.h"
#include "core/sha256.h"

namespace musicboard {

// 仅规范路径和计算名称，不创建文件，也不改变安装状态。
[[nodiscard]] inline bool transaction_mutex_name(const std::filesystem::path& root,
                                                  std::wstring& name, Failure& failure) {
    if (!root.is_absolute()) {
        failure.set(fail_code::invalid_argument, "安装事务需要完整的安装目录");
        return false;
    }
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(root, error);
    if (error) {
        failure.set(fail_code::file_open_failed, "无法规范安装事务目录: " + error.message());
        return false;
    }
    auto normalized = canonical.lexically_normal().native();
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (normalized.starts_with(L"\\\\?\\UNC\\")) normalized = L"\\\\" + normalized.substr(8);
    else if (normalized.starts_with(L"\\\\?\\") && normalized.size() > 5 && normalized[5] == L':') normalized.erase(0, 4);
    while (normalized.size() > 3 && normalized.back() == L'\\') normalized.pop_back();
    const int count = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        normalized.data(), static_cast<int>(normalized.size()), nullptr, 0, nullptr, nullptr, 0);
    if (count <= 0) {
        failure.set(fail_code::platform_api_failed, "无法统一安装目录大小写");
        return false;
    }
    std::wstring folded(static_cast<std::size_t>(count), L'\0');
    if (::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, normalized.data(),
        static_cast<int>(normalized.size()), folded.data(), count, nullptr, nullptr, 0) != count) {
        failure.set(fail_code::platform_api_failed, "无法统一安装目录大小写");
        return false;
    }
    std::string digest;
    if (!sha256_hex(reinterpret_cast<const std::uint8_t*>(folded.data()),
                    folded.size() * sizeof(wchar_t), digest, failure)) return false;
    name = L"Local\\MusicBoard.Setup.Transaction.";
    for (char character : digest) name += static_cast<wchar_t>(static_cast<unsigned char>(character));
    return true;
}

class SetupTransactionLock final {
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
public:
    SetupTransactionLock() = default;
    SetupTransactionLock(const SetupTransactionLock&) = delete;
    SetupTransactionLock& operator=(const SetupTransactionLock&) = delete;
    ~SetupTransactionLock() {
        if (owned_) ::ReleaseMutex(mutex_);
        if (mutex_) ::CloseHandle(mutex_);
    }
    [[nodiscard]] bool acquire(const std::filesystem::path& root, Failure& failure) {
        std::wstring name;
        if (!transaction_mutex_name(root, name, failure)) return false;
        mutex_ = ::CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex_) {
            failure.set(fail_code::platform_api_failed, "无法建立安装事务锁（" + std::to_string(::GetLastError()) + "）");
            return false;
        }
        const DWORD result = ::WaitForSingleObject(mutex_, 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) { owned_ = true; return true; }
        failure.set(result == WAIT_TIMEOUT ? "install.transaction_busy" : fail_code::platform_api_failed,
            result == WAIT_TIMEOUT ? "此目录已有安装或卸载正在进行，请等待完成后重试" :
            "无法取得安装事务锁（" + std::to_string(::GetLastError()) + "）");
        return false;
    }
};

} // namespace musicboard
