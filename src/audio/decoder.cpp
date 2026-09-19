#include "audio/decoder.h"
#include <cmath>

#include <string>

#include "audio/decoder_backends.h"
#include "core/file_bytes.h"
#include "core/path_utf8.h"

namespace musicboard {
namespace {

// 嗅探只需前 64 字节；多读一点也无所谓，但没必要。
constexpr std::size_t kSniffBytes = 64;

[[nodiscard]] bool sniff(const std::filesystem::path& path, ContainerProbe& out, Failure& failure) {
    std::vector<std::uint8_t> head;
    if (!read_file_head(path, kSniffBytes, head, failure)) {
        return false;
    }
    out = probe_container(head);
    return true;
}

}  // namespace

bool container_is_decodable(AudioContainer container) noexcept {
    switch (container) {
        // Media Foundation 覆盖的
        case AudioContainer::wav:
        case AudioContainer::mp3:
        case AudioContainer::mp4:
        case AudioContainer::wma:
        case AudioContainer::aiff:
        case AudioContainer::flac:
            return true;
        // stb_vorbis 覆盖的
        case AudioContainer::ogg:
            return true;
        case AudioContainer::unknown:
            return false;
    }
    return false;
}

bool probe_audio_file(const std::filesystem::path& path, AudioInfo& info, Failure& failure) {
    ContainerProbe probe;
    if (!sniff(path, probe, failure)) {
        return false;
    }

    // 容器未知也要给出明确结论，而不是让下游去猜
    if (probe.container == AudioContainer::unknown) {
        failure.set(fail_code::unsupported_format, path_to_utf8(path) + "：" + std::string(probe.description));
        return false;
    }

    bool ok = false;
    if (probe.container == AudioContainer::ogg) {
        ok = detail::ogg_probe(path, info, failure);
    } else {
        ok = detail::mf_probe(path, info, failure);
    }
    if (!ok) {
        // 后端失败时补上容器信息 —— 最内层的原因已经写在 failure 里了，
        // 这里只在 code 为空时兜底，不覆盖更有价值的原始原因。
        if (failure.code.empty()) {
            failure.set(fail_code::decode_failed,
                        path_to_utf8(path) + " 容器=" + container_name(probe.container) + " 探测失败");
        }
        return false;
    }
    info.container = probe.container;
    return true;
}

bool read_audio_chunks(const std::filesystem::path& path, AudioChunkConsumer& consumer,
                       Failure& failure, const std::atomic<bool>* cancelled, double start_seconds) {
    if (!std::isfinite(start_seconds) || start_seconds < 0.0) {
        failure.set(fail_code::invalid_argument, path_to_utf8(path) + " invalid stream start"); return false;
    }
    ContainerProbe probe;
    if (!sniff(path, probe, failure)) return false;
    if (probe.container == AudioContainer::unknown) {
        failure.set(fail_code::unsupported_format, path_to_utf8(path) + ": " + std::string(probe.description));
        return false;
    }
    const bool ok = probe.container == AudioContainer::ogg
        ? detail::ogg_read_chunks(path, consumer, failure, cancelled, start_seconds)
        : detail::mf_read_chunks(path, consumer, failure, cancelled, start_seconds);
    if (!ok && failure.empty()) {
        failure.set(fail_code::decode_failed, path_to_utf8(path) + " chunk decoding failed");
    }
    return ok;
}

bool decode_audio_file(const std::filesystem::path& path, DecodedAudio& audio, Failure& failure,
                       const std::atomic<bool>* cancelled) {
    class Collector final : public AudioChunkConsumer {
    public:
        std::string source;
        DecodedAudio value;
        [[nodiscard]] bool consume(const AudioFormat& format, const std::int16_t* samples,
                                   std::size_t frames, Failure& problem) override {
            const std::size_t count = frames * format.channels;
            // XAudio2 accepts at most 2 GiB per buffer. Fail before unbounded allocation.
            constexpr std::size_t max_samples = 0x80000000ull / sizeof(std::int16_t);
            if (count > max_samples - value.samples.size()) {
                problem.set(fail_code::file_too_large, source + " PCM exceeds the XAudio2 2 GiB buffer limit");
                return false;
            }
            value.format = format;
            value.samples.insert(value.samples.end(), samples, samples + count);
            return true;
        }
    } collector;
    collector.source = path_to_utf8(path);
    audio = DecodedAudio{};
    if (!read_audio_chunks(path, collector, failure, cancelled)) return false;
    collector.value.duration_seconds = static_cast<double>(collector.value.frame_count()) /
                                       collector.value.format.sample_rate;
    ContainerProbe probe;
    if (!sniff(path, probe, failure)) return false;
    collector.value.backend = probe.container == AudioContainer::ogg ? "stb_vorbis" : "media_foundation";
    audio = std::move(collector.value);
    return true;
}

}  // namespace musicboard
