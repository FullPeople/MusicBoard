#pragma once

#include "audio/decoder.h"

// 内部头文件：后端之间不互相依赖，只被 decoder.cpp 分派。
// 每个后端只负责"我这类容器"，不负责判断该不该由我来处理（那是分派的事）。

namespace musicboard::detail {

// Media Foundation：mp3 / wav / m4a(aac,alac) / wma / flac(Win10+ 视系统而定)
// 实测本机：mp3 与 wav 通过；ogg 返回 MF_E_BYTESOURCE_UNKNOWN_TYPE。
[[nodiscard]] bool mf_probe(const std::filesystem::path& path, AudioInfo& info, Failure& failure);
[[nodiscard]] bool mf_read_chunks(const std::filesystem::path& path, AudioChunkConsumer& consumer,
                                       Failure& failure, const std::atomic<bool>* cancelled, double start_seconds);

// stb_vorbis：ogg。只走内存解码 —— 见 third_party/stb/README.md
[[nodiscard]] bool ogg_probe(const std::filesystem::path& path, AudioInfo& info, Failure& failure);
[[nodiscard]] bool ogg_read_chunks(const std::filesystem::path& path, AudioChunkConsumer& consumer,
                                       Failure& failure, const std::atomic<bool>* cancelled, double start_seconds);

}  // namespace musicboard::detail
