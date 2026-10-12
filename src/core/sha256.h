#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// SHA-256
//
// 用系统自带的 CNG（bcrypt）而不是自己实现或引第三方：
//   · 校验安装包完整性的东西不该自己手写密码学原语
//   · 不引入依赖，包就是单 exe
//
// 输出一律是小写十六进制。校验时大小写不敏感（比较前统一转小写）——
// 但**生成**时只用一种写法，免得清单文件里两种混着出现。
// ============================================================================

inline constexpr std::size_t sha256_digest_bytes = 32;
inline constexpr std::size_t sha256_hex_length = 64;

// 一段内存的 SHA256，返回小写十六进制
[[nodiscard]] bool sha256_hex(const std::uint8_t* data, std::size_t size, std::string& out,
                             Failure& failure);

// 一个文件的 SHA256。**流式读取**：安装包里的文件可能几百 MB，
// 整个读进内存既浪费又会在低内存机器上失败。
[[nodiscard]] bool sha256_file_hex(const std::filesystem::path& path, std::string& out,
                                  Failure& failure);

// 校验一个十六进制摘要串的格式（长度 64、全是十六进制字符）。大小写不限。
[[nodiscard]] bool is_hex_digest(std::string_view text) noexcept;

// 把两个摘要按大小写不敏感比较
[[nodiscard]] bool digest_equals(std::string_view a, std::string_view b) noexcept;

}  // namespace musicboard
