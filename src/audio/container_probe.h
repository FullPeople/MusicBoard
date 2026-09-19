#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 容器嗅探
//
// 按内容判断，不按扩展名。理由：扩展名是用户随手改的，而"该怎么解"取决于
// 文件里到底是什么。一个名叫 .mp3 的 ogg 交给 Media Foundation 只会得到
// MF_E_BYTESOURCE_UNKNOWN_TYPE，然后被误判成"文件坏了"。
//
// 这个模块是纯函数：输入若干字节，输出容器类型。测试完全离线。
// ============================================================================

enum class AudioContainer : std::uint8_t {
    unknown = 0,
    wav,      // RIFF....WAVE
    mp3,      // ID3v2 标签，或 MPEG 帧同步
    ogg,      // OggS（Vorbis / Opus 等）
    flac,     // fLaC
    mp4,      // ....ftyp（m4a / aac / alac）
    wma,      // ASF GUID
    aiff,     // FORM....AIFF
};

[[nodiscard]] const char* container_name(AudioContainer container) noexcept;

struct ContainerProbe final {
    AudioContainer container = AudioContainer::unknown;
    // 给人看的一句话，写日志/错误信息用，例如 "OggS (Vorbis/Opus 容器)"
    std::string_view description;
};

// 只需前 64 字节即可判定；给更少会返回 unknown 而不是猜。
[[nodiscard]] ContainerProbe probe_container(const std::vector<std::uint8_t>& head) noexcept;

}  // namespace musicboard
