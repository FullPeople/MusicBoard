#include "core/file_bytes.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "core/path_utf8.h"

namespace musicboard {
namespace {

// 统一用 Win32 宽字符 API 打开文件，不走 std::ifstream(path) ——
// 后者的文件名参数在 MSVC 上是窄字符，中文路径必坏。
class UniqueFile final {
public:
    explicit UniqueFile(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueFile() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }
    UniqueFile(const UniqueFile&) = delete;
    UniqueFile& operator=(const UniqueFile&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

[[nodiscard]] UniqueFile open_for_read(const std::filesystem::path& path, Failure& failure) {
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            failure.set(fail_code::file_not_found, path_to_utf8(path));
        } else {
            failure.set(fail_code::file_open_failed,
                        path_to_utf8(path) + " (Win32 错误 " + std::to_string(err) + ")");
        }
    }
    return UniqueFile(handle);
}

}  // namespace

bool read_file_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                     Failure& failure, std::uint64_t max_bytes) {
    out.clear();
    UniqueFile file = open_for_read(path, failure);
    if (!file.valid()) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size)) {
        failure.set(fail_code::platform_api_failed,
                    "GetFileSizeEx 失败 " + path_to_utf8(path) + " (Win32 错误 " +
                        std::to_string(::GetLastError()) + ")");
        return false;
    }
    if (size.QuadPart < 0) {
        failure.set(fail_code::file_read_failed, "文件大小为负 " + path_to_utf8(path));
        return false;
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
        failure.set(fail_code::file_too_large,
                    path_to_utf8(path) + " 大小 " + std::to_string(size.QuadPart) +
                        " 字节，超过上限 " + std::to_string(max_bytes));
        return false;
    }

    out.resize(static_cast<std::size_t>(size.QuadPart));
    if (out.empty()) {
        failure.set(fail_code::file_read_failed, "文件为空 " + path_to_utf8(path));
        return false;
    }

    std::size_t total_read = 0;
    while (total_read < out.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - total_read, 1u << 20));
        if (!::ReadFile(file.get(), out.data() + total_read, want, &chunk, nullptr)) {
            failure.set(fail_code::file_read_failed,
                        "ReadFile 失败 " + path_to_utf8(path) + " (Win32 错误 " +
                            std::to_string(::GetLastError()) + ")");
            out.clear();
            return false;
        }
        if (chunk == 0) {
            break;   // 文件在读取期间被截断
        }
        total_read += chunk;
    }
    if (total_read != out.size()) {
        failure.set(fail_code::file_read_failed,
                    "读取长度不符 " + path_to_utf8(path) + " 期望 " + std::to_string(out.size()) +
                        " 实际 " + std::to_string(total_read));
        out.clear();
        return false;
    }
    return true;
}

bool read_file_head(const std::filesystem::path& path, std::size_t head_bytes,
                    std::vector<std::uint8_t>& out, Failure& failure) {
    out.clear();
    if (head_bytes == 0) {
        failure.set(fail_code::invalid_argument, "head_bytes 不能为 0");
        return false;
    }

    UniqueFile file = open_for_read(path, failure);
    if (!file.valid()) {
        return false;
    }

    out.resize(head_bytes);
    DWORD read = 0;
    if (!::ReadFile(file.get(), out.data(), static_cast<DWORD>(head_bytes), &read, nullptr)) {
        failure.set(fail_code::file_read_failed,
                    "ReadFile 失败 " + path_to_utf8(path) + " (Win32 错误 " +
                        std::to_string(::GetLastError()) + ")");
        out.clear();
        return false;
    }
    out.resize(read);   // 文件比 head_bytes 短是正常情况，按实际长度返回
    if (out.empty()) {
        failure.set(fail_code::file_read_failed, "文件为空 " + path_to_utf8(path));
        return false;
    }
    return true;
}

}  // namespace musicboard
