#include "audio/container_probe.h"

namespace musicboard {
namespace {

[[nodiscard]] bool matches(const std::vector<std::uint8_t>& data, std::size_t offset,
                           std::string_view magic) noexcept {
    if (offset + magic.size() > data.size()) {
        return false;
    }
    for (std::size_t i = 0; i < magic.size(); ++i) {
        if (data[offset + i] != static_cast<std::uint8_t>(magic[i])) {
            return false;
        }
    }
    return true;
}

// MPEG 音频帧同步：11 个 1。用于识别没有 ID3 标签的裸 MP3。
[[nodiscard]] bool looks_like_mpeg_frame(const std::vector<std::uint8_t>& data,
                                         std::size_t offset) noexcept {
    if (offset + 1 >= data.size()) {
        return false;
    }
    const std::uint8_t b0 = data[offset];
    const std::uint8_t b1 = data[offset + 1];
    if ((b0 & 0xFF) != 0xFF || (b1 & 0xE0) != 0xE0) {
        return false;
    }
    // 层字段不能是保留值
    const std::uint8_t layer = (b1 >> 1) & 0x03;
    return layer != 0;
}

}  // namespace

const char* container_name(AudioContainer container) noexcept {
    switch (container) {
        case AudioContainer::unknown: return "unknown";
        case AudioContainer::wav:     return "wav";
        case AudioContainer::mp3:     return "mp3";
        case AudioContainer::ogg:     return "ogg";
        case AudioContainer::flac:    return "flac";
        case AudioContainer::mp4:     return "mp4";
        case AudioContainer::wma:     return "wma";
        case AudioContainer::aiff:    return "aiff";
    }
    return "unknown";
}

ContainerProbe probe_container(const std::vector<std::uint8_t>& head) noexcept {
    constexpr std::size_t kMinimumUsefulBytes = 12;
    if (head.size() < kMinimumUsefulBytes) {
        return {AudioContainer::unknown, "字节不足，无法判定"};
    }

    // RIFF....WAVE —— 注意 WAVE 在偏移 8，不是 0
    if (matches(head, 0, "RIFF") && matches(head, 8, "WAVE")) {
        return {AudioContainer::wav, "RIFF/WAVE"};
    }
    if (matches(head, 0, "FORM") && (matches(head, 8, "AIFF") || matches(head, 8, "AIFC"))) {
        return {AudioContainer::aiff, "IFF/AIFF"};
    }
    if (matches(head, 0, "OggS")) {
        return {AudioContainer::ogg, "OggS（Vorbis/Opus 容器）"};
    }
    if (matches(head, 0, "fLaC")) {
        return {AudioContainer::flac, "FLAC"};
    }
    if (matches(head, 0, "ID3")) {
        return {AudioContainer::mp3, "ID3v2 标签"};
    }
    // ASF GUID：30 26 B2 75 8E 66 CF 11 A6 D9 00 AA 00 62 CE 6C
    if (matches(head, 0, std::string_view("\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8))) {
        return {AudioContainer::wma, "ASF/WMA"};
    }
    // ISO 基媒体：ftyp 在偏移 4
    if (matches(head, 4, "ftyp")) {
        return {AudioContainer::mp4, "ISO-BMFF（m4a/aac/alac）"};
    }
    // 裸 MP3：帧同步。放在最后，避免误吞别的格式
    if (looks_like_mpeg_frame(head, 0)) {
        return {AudioContainer::mp3, "MPEG 帧同步（无 ID3 标签）"};
    }

    return {AudioContainer::unknown, "未识别的容器"};
}

}  // namespace musicboard
