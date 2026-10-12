#include "audio/prepared_audio.h"
#include <algorithm>
#include <cmath>
#include "core/path_utf8.h"

namespace musicboard {
bool prepare_audio(const std::filesystem::path& path, const AudioInfo& info,
    std::shared_ptr<const PreparedAudio>& prepared, Failure& failure,
    const std::atomic<bool>* cancelled, const std::function<void(double)>& progress) {
    prepared.reset();
    if (info.format.channels == 0 || info.format.sample_rate == 0 ||
        !std::isfinite(info.duration_seconds) || info.duration_seconds <= 0.0) {
        failure.set(fail_code::invalid_argument, path_to_utf8(path) + " invalid preload metadata");
        return false;
    }
    auto value = std::make_shared<PreparedAudio>();
    value->path = path;
    value->head.format = info.format;
    value->envelope.resize(320);
    struct Consumer final : AudioChunkConsumer {
        PreparedAudio& value;
        const AudioInfo& info;
        const std::function<void(double)>& progress;
        std::uint64_t retained_frames;
        Consumer(PreparedAudio& v, const AudioInfo& i, const std::function<void(double)>& p)
            : value(v), info(i), progress(p), retained_frames(static_cast<std::uint64_t>(
                i.duration_seconds <= 30.0 ? 31.0 * i.format.sample_rate : preload_head_seconds * i.format.sample_rate)) {}
        [[nodiscard]] bool finished() const noexcept override {
            return info.duration_seconds > 30.0 && value.total_frames >= retained_frames;
        }
        bool consume(const AudioFormat& format, const std::int16_t* samples,
                     std::size_t frames, Failure& f) override {
            if (format.channels != info.format.channels || format.sample_rate != info.format.sample_rate) {
                f.set(fail_code::decode_failed, path_to_utf8(value.path) + " preload format changed");
                return false;
            }
            if (value.total_frames < retained_frames) {
                const auto count = std::min<std::uint64_t>(frames, retained_frames - value.total_frames);
                value.head.samples.insert(value.head.samples.end(), samples, samples + count * format.channels);
            }
            const double expected = info.duration_seconds * format.sample_rate;
            for (std::size_t frame = 0; info.duration_seconds <= 30.0 && frame < frames; ++frame) {
                const auto bucket = std::min<std::size_t>(319, static_cast<std::size_t>(
                    static_cast<double>(value.total_frames + frame) / expected * 320.0));
                for (std::size_t channel = 0; channel < format.channels; ++channel) {
                    value.envelope[bucket] = std::max(value.envelope[bucket],
                        std::abs(static_cast<float>(samples[frame * format.channels + channel])) / 32768.0f);
                }
            }
            value.total_frames += frames;
            if (progress) progress(std::min(0.999, static_cast<double>(value.total_frames) /
                (info.duration_seconds <= 30.0 ? expected : static_cast<double>(retained_frames))));
            return true;
        }
    } consumer(*value, info, progress);
    if (!read_audio_chunks(path, consumer, failure, cancelled)) return false;
    value->head.duration_seconds = static_cast<double>(value->head.frame_count()) / info.format.sample_rate;
    value->complete = info.duration_seconds <= 30.0 && value->total_frames == value->head.frame_count();
    if (!value->complete) {
        value->total_frames = static_cast<std::uint64_t>(std::llround(info.duration_seconds * info.format.sample_rate));
        value->envelope.clear();
    }
    const float peak = value->envelope.empty() ? 0.0f : *std::max_element(value->envelope.begin(), value->envelope.end());
    if (peak > 0.0f) for (float& v : value->envelope) v /= peak;
    value->head.samples.shrink_to_fit();
    prepared = std::move(value);
    return true;
}
} // namespace musicboard
