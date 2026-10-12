#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 单一 UTF-8 边界
//
// 这是整个项目最容易出错、也最不该出错的一处，所以单独成文件并写清楚原因。
//
// Windows 上 std::filesystem::path::string() 走活动代码页（中文系统是 936/GBK）。
// 于是同一个中文文件名会得到两种字节序列：
//
//     path.string()  -> 19 字节（GBK）
//     path.u8string()-> 26 字节（UTF-8）
//
// 只要有任何一处把这两种混起来，日志会乱码、JSON 会写坏、用户会看到问号。
// 实测（见 tests/core_tests.cpp）这个差异在本机上是可复现的。
//
// 规则：
//   · 内部一律用 std::filesystem::path 与宽字符 Win32 API
//   · 只在"给人看"或"序列化"的边界上转 UTF-8，且必须经过下面这几个函数
//   · 不允许在别处直接调用 path.string()
// ============================================================================

// 把路径转成 UTF-8（用于日志、JSON、UI 文本）
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

// 把 UTF-8 转成路径。输入不是合法 UTF-8 时返回失败，绝不猜编码。
[[nodiscard]] bool path_from_utf8(std::string_view utf8, std::filesystem::path& out, Failure& failure);

// 宽字符串 ↔ UTF-8。给需要直接调 Win32 宽字符 API 的地方用。
[[nodiscard]] std::string wide_to_utf8(std::wstring_view wide);
[[nodiscard]] bool utf8_to_wide(std::string_view utf8, std::wstring& out, Failure& failure);

}  // namespace musicboard
