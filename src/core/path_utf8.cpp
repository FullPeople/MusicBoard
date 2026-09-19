#include "core/path_utf8.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace musicboard {
namespace {

// UTF-8 → 宽字符。MB_ERR_INVALID_CHARS 让非法 UTF-8 直接失败，而不是替换成 U+FFFD。
[[nodiscard]] bool multibyte_to_wide(std::string_view utf8, UINT code_page,
                                     DWORD flags, std::wstring& out, Failure& failure) {
    if (utf8.empty()) {
        out.clear();
        return true;
    }
    const int needed = MultiByteToWideChar(code_page, flags, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    if (needed <= 0) {
        failure.set(fail_code::invalid_argument, "输入不是合法的 UTF-8 字节序列");
        return false;
    }
    out.resize(static_cast<size_t>(needed));
    const int written = MultiByteToWideChar(code_page, flags, utf8.data(), static_cast<int>(utf8.size()),
                                            out.data(), needed);
    if (written != needed) {
        failure.set(fail_code::platform_api_failed,
                    "MultiByteToWideChar 写入长度不符，期望 " + std::to_string(needed) +
                        " 实际 " + std::to_string(written));
        return false;
    }
    return true;
}

}  // namespace

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

bool path_from_utf8(std::string_view utf8, std::filesystem::path& out, Failure& failure) {
    std::wstring wide;
    if (!multibyte_to_wide(utf8, CP_UTF8, MB_ERR_INVALID_CHARS, wide, failure)) {
        return false;
    }
    out = std::filesystem::path(std::move(wide));
    return true;
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                            out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        return {};
    }
    return out;
}

bool utf8_to_wide(std::string_view utf8, std::wstring& out, Failure& failure) {
    return multibyte_to_wide(utf8, CP_UTF8, MB_ERR_INVALID_CHARS, out, failure);
}

}  // namespace musicboard
