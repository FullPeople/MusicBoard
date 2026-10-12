#include "core/sha256.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cctype>
#include <vector>

#include "core/path_utf8.h"

#pragma comment(lib, "bcrypt.lib")

namespace musicboard {
namespace {

constexpr std::size_t kChunkBytes = 256u * 1024u;   // 256 KB 一块

[[nodiscard]] std::string to_hex(const std::uint8_t* digest, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kDigits[digest[i] >> 4];
        out[i * 2 + 1] = kDigits[digest[i] & 0x0F];
    }
    return out;
}

// CNG 的算法句柄用一个小的 RAII 包住：中间任何一步失败都要关掉它，
// 手写 goto cleanup 在有好几个失败分支时很容易漏一处。
class AlgorithmHandle final {
public:
    ~AlgorithmHandle() {
        if (handle_ != nullptr) {
            ::BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }
    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
    AlgorithmHandle() = default;

    [[nodiscard]] bool open(Failure& failure) {
        const NTSTATUS status =
            ::BCryptOpenAlgorithmProvider(&handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status < 0) {
            failure.set(fail_code::platform_api_failed,
                        "打开 SHA-256 算法失败（NTSTATUS 0x" +
                            std::to_string(static_cast<unsigned long>(status)) + "）");
            return false;
        }
        return true;
    }
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class HashHandle final {
public:
    ~HashHandle() {
        if (handle_ != nullptr) {
            ::BCryptDestroyHash(handle_);
        }
    }
    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;
    HashHandle() = default;

    [[nodiscard]] BCRYPT_HASH_HANDLE* address() noexcept { return &handle_; }
    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

}  // namespace

bool sha256_hex(const std::uint8_t* data, std::size_t size, std::string& out, Failure& failure) {
    out.clear();

    AlgorithmHandle algorithm;
    if (!algorithm.open(failure)) {
        return false;
    }

    // 哈希对象的大小由算法决定，要先问出来（不能假设）
    DWORD object_size = 0;
    DWORD written = 0;
    NTSTATUS status = ::BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
                                         reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                         &written, 0);
    if (status < 0 || object_size == 0) {
        failure.set(fail_code::platform_api_failed, "取 SHA-256 对象长度失败");
        return false;
    }

    std::vector<std::uint8_t> object(object_size);
    HashHandle hash;
    status = ::BCryptCreateHash(algorithm.get(), hash.address(), object.data(), object_size, nullptr,
                               0, 0);
    if (status < 0) {
        failure.set(fail_code::platform_api_failed, "创建 SHA-256 哈希对象失败");
        return false;
    }

    if (size > 0) {
        status = ::BCryptHashData(hash.get(), const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0);
        if (status < 0) {
            failure.set(fail_code::platform_api_failed, "SHA-256 处理数据失败");
            return false;
        }
    }

    std::array<std::uint8_t, sha256_digest_bytes> digest{};
    status = ::BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (status < 0) {
        failure.set(fail_code::platform_api_failed, "SHA-256 收尾失败");
        return false;
    }

    out = to_hex(digest.data(), digest.size());
    return true;
}

bool sha256_file_hex(const std::filesystem::path& path, std::string& out, Failure& failure) {
    out.clear();

    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        failure.set(fail_code::file_open_failed,
                    "无法打开 " + path_to_utf8(path) + "（Win32 错误 " +
                        std::to_string(::GetLastError()) + "）");
        return false;
    }

    AlgorithmHandle algorithm;
    if (!algorithm.open(failure)) {
        ::CloseHandle(file);
        return false;
    }
    DWORD object_size = 0;
    DWORD written = 0;
    NTSTATUS status = ::BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
                                         reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                         &written, 0);
    if (status < 0 || object_size == 0) {
        failure.set(fail_code::platform_api_failed, "取 SHA-256 对象长度失败");
        ::CloseHandle(file);
        return false;
    }
    std::vector<std::uint8_t> object(object_size);
    HashHandle hash;
    status = ::BCryptCreateHash(algorithm.get(), hash.address(), object.data(), object_size, nullptr,
                               0, 0);
    if (status < 0) {
        failure.set(fail_code::platform_api_failed, "创建 SHA-256 哈希对象失败");
        ::CloseHandle(file);
        return false;
    }

    // 流式读：安装包里的文件可能几百 MB，整个读进内存既浪费又会在低内存机器上失败
    std::vector<std::uint8_t> buffer(kChunkBytes);
    for (;;) {
        DWORD read = 0;
        if (::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ==
            FALSE) {
            failure.set(fail_code::file_read_failed,
                        "读取 " + path_to_utf8(path) + " 失败（Win32 错误 " +
                            std::to_string(::GetLastError()) + "）");
            ::CloseHandle(file);
            return false;
        }
        if (read == 0) {
            break;
        }
        status = ::BCryptHashData(hash.get(), buffer.data(), read, 0);
        if (status < 0) {
            failure.set(fail_code::platform_api_failed, "SHA-256 处理数据失败");
            ::CloseHandle(file);
            return false;
        }
    }
    ::CloseHandle(file);

    std::array<std::uint8_t, sha256_digest_bytes> digest{};
    status = ::BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (status < 0) {
        failure.set(fail_code::platform_api_failed, "SHA-256 收尾失败");
        return false;
    }
    out = to_hex(digest.data(), digest.size());
    return true;
}

bool is_hex_digest(std::string_view text) noexcept {
    if (text.size() != sha256_hex_length) {
        return false;
    }
    for (const char ch : text) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                         (ch >= 'A' && ch <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

bool digest_equals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto left = static_cast<unsigned char>(a[i]);
        const auto right = static_cast<unsigned char>(b[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

}  // namespace musicboard
