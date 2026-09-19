#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 整文件读取
//
// 只给"确实需要全部字节"的场景用（解码器的输入）。
// 扫描曲库时不要用这个 —— 那正是要避免的"把整个曲库读进内存"。
// ============================================================================

// 默认上限 512MB：单个音频文件不该超过这个量级。超过就明确失败，
// 而不是让一个坏文件把进程的内存吃光。
inline constexpr std::uint64_t kDefaultMaxReadBytes = 512ull * 1024ull * 1024ull;

[[nodiscard]] bool read_file_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                                   Failure& failure,
                                   std::uint64_t max_bytes = kDefaultMaxReadBytes);

// 只读文件开头若干字节。容器嗅探用这个，避免为了看 8 个字节读进整个文件。
[[nodiscard]] bool read_file_head(const std::filesystem::path& path, std::size_t head_bytes,
                                  std::vector<std::uint8_t>& out, Failure& failure);

}  // namespace musicboard
