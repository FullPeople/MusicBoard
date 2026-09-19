#include "audio/waveform.h"
#include "core/path_utf8.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace musicboard {

bool extract_waveform_file(const std::filesystem::path& path, std::vector<float>& envelope,
                           Failure& failure, const std::atomic<bool>* cancelled) {
    envelope.clear();
    AudioInfo info;
    if (!probe_audio_file(path, info, failure)) return false;
    const double estimated_frames = info.duration_seconds * info.format.sample_rate;
    if (!std::isfinite(estimated_frames) || estimated_frames < 1.0 || estimated_frames > 1.0e12) {
        failure.set(fail_code::decode_failed, path_to_utf8(path) + " invalid waveform duration");
        return false;
    }
    class Peaks final : public AudioChunkConsumer {
    public:
        std::uint64_t expected = 0;
        std::uint64_t cursor = 0;
        std::vector<float> values = std::vector<float>(kDefaultWaveformBuckets, 0.0f);
        [[nodiscard]] bool consume(const AudioFormat& format, const std::int16_t* samples,
                                   std::size_t frames, Failure&) override {
            for (std::size_t frame = 0; frame < frames; ++frame, ++cursor) {
                const std::size_t bucket = static_cast<std::size_t>(std::min<std::uint64_t>(
                    ((cursor + 1) * values.size() - 1) / expected, values.size() - 1));
                for (std::size_t channel = 0; channel < format.channels; ++channel) {
                    values[bucket] = std::max(values[bucket],
                        std::fabs(static_cast<float>(samples[frame * format.channels + channel])));
                }
            }
            return true;
        }
    } peaks;
    peaks.expected = static_cast<std::uint64_t>(std::llround(estimated_frames));
    if (!read_audio_chunks(path, peaks, failure, cancelled)) return false;
    const float maximum = *std::max_element(peaks.values.begin(), peaks.values.end());
    if (maximum > 0.0f) for (float& value : peaks.values) value /= maximum;
    envelope = std::move(peaks.values);
    return true;
}

std::vector<float> compute_waveform_envelope(const std::int16_t* samples,
                                             std::size_t frame_count, std::size_t channels,
                                             std::size_t buckets, Failure& failure) {
    if (buckets == 0) {
        failure.set(fail_code::invalid_argument, "波形桶数不能为 0");
        return {};
    }
    if (channels == 0) {
        failure.set(fail_code::invalid_argument,
                    "声道数为 0，无法解释交错的样本序列");
        return {};
    }
    if (frame_count == 0 || samples == nullptr) {
        // 空音频给一条平坦的线，不算失败 —— "什么都没有"本身是合法结果。
        // 但要明确返回全 0，而不是让调用方拿到一个空 vector 之后自己去猜。
        return std::vector<float>(buckets, 0.0f);
    }

    std::vector<float> envelope(buckets, 0.0f);
    float peak_overall = 0.0f;

    // 每个桶覆盖的帧数。用整数除法会让最后的尾巴被丢掉，
    // 所以按"这一桶的帧区间"直接算边界，保证所有帧都被扫到。
    for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
        const std::size_t begin_frame = frame_count * bucket / buckets;
        const std::size_t end_frame = frame_count * (bucket + 1) / buckets;

        float peak = 0.0f;
        for (std::size_t frame = begin_frame; frame < end_frame; ++frame) {
            const std::size_t base = frame * channels;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                // 取绝对值再归一化：-32768 的绝对值是 32768，用 float 接得住
                const float value =
                    std::fabs(static_cast<float>(samples[base + channel])) / 32768.0f;
                if (value > peak) {
                    peak = value;
                }
            }
        }
        envelope[bucket] = peak;
        if (peak > peak_overall) {
            peak_overall = peak;
        }
    }

    if (peak_overall <= 0.0f) {
        // 全静音：保持全 0。**不**伪造一条底噪，也不除以 0。
        return envelope;
    }
    for (float& value : envelope) {
        value /= peak_overall;
    }
    return envelope;
}

std::vector<float> compute_waveform_envelope(const DecodedAudio& audio, std::size_t buckets,
                                             Failure& failure) {
    return compute_waveform_envelope(audio.samples.data(), audio.frame_count(), audio.format.channels,
                                     buckets, failure);
}

float amplitude_at(const std::vector<float>& envelope, double progress) noexcept {
    if (envelope.empty()) {
        return 0.0f;
    }
    double clamped = progress;
    if (clamped < 0.0) {
        clamped = 0.0;
    }
    if (clamped > 1.0) {
        clamped = 1.0;
    }
    const double scaled = clamped * static_cast<double>(envelope.size());
    auto index = static_cast<std::size_t>(scaled);
    if (index >= envelope.size()) {
        index = envelope.size() - 1;   // progress == 1.0 时落在最后一桶上
    }
    return envelope[index];
}

}  // namespace musicboard
