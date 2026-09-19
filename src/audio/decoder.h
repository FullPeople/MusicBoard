#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "audio/container_probe.h"
#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 解码器契约
//
// 分成两个层次，这是整个音频方案的关键设计：
//
//   probe_audio_file  —— 轻量。只读出格式与时长，不做完整解码。
//                        扫描曲库时用它。3.7 GB 的库必须能秒级列出来。
//   decode_audio_file —— 完整。解成 PCM16 交错样本。
//                        只有真正要播某一首时才用它。
//
// 为什么必须分开：Godot 原型在扫描阶段就把每个文件整个读进内存
// （audio_loader.gd 的 file.get_buffer(file.get_length())）。
// 实测曲库是 3.7 GB / 491 个文件 —— 那样做的常驻内存是 GB 级。
// 把"看过"和"取出"拆成两个操作，这个问题在接口层面就不存在了。
// ============================================================================

struct AudioFormat final {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 16;
};

struct AudioInfo final {
    AudioContainer container = AudioContainer::unknown;
    AudioFormat format;
    double duration_seconds = 0.0;
};

struct DecodedAudio final {
    AudioFormat format;
    double duration_seconds = 0.0;
    // PCM16 交错样本：左、右、左、右……
    std::vector<std::int16_t> samples;
    // 来自哪个后端（"media_foundation" / "stb_vorbis"），写日志与做覆盖率统计用
    const char* backend = "";

    [[nodiscard]] std::size_t frame_count() const noexcept {
        return format.channels == 0 ? 0 : samples.size() / format.channels;
    }
};

// A streaming consumer never owns the decoder's temporary PCM buffer.
class AudioChunkConsumer {
public:
    virtual ~AudioChunkConsumer() = default;
    // A prefix consumer can finish successfully without decoding the tail.
    [[nodiscard]] virtual bool finished() const noexcept { return false; }
    [[nodiscard]] virtual bool consume(const AudioFormat& format, const std::int16_t* samples,
                                       std::size_t frames, Failure& failure) = 0;
};

// Shared backend traversal for playback and on-demand waveform extraction.
[[nodiscard]] bool read_audio_chunks(const std::filesystem::path& path,
                                     AudioChunkConsumer& consumer, Failure& failure,
                                     const std::atomic<bool>* cancelled = nullptr, double start_seconds = 0.0);

// 只探格式与时长，不解码。适合遍历整个曲库。
[[nodiscard]] bool probe_audio_file(const std::filesystem::path& path, AudioInfo& info, Failure& failure);

// 完整解码成 PCM16。适合播放前调用一次。
[[nodiscard]] bool decode_audio_file(const std::filesystem::path& path, DecodedAudio& audio,
                                     Failure& failure, const std::atomic<bool>* cancelled = nullptr);

// 该容器类型当前是否有可用后端。用于扫描时给出"这个文件播不了"的早期结论，
// 而不是等到用户点了才失败。
[[nodiscard]] bool container_is_decodable(AudioContainer container) noexcept;

}  // namespace musicboard
