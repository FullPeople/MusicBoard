#pragma once
#include <string>
#include "audio/prepared_audio.h"

namespace musicboard {
struct OnlineAudioSource final {
    std::filesystem::path decoder;
    std::string url;
    std::string headers;
    double duration_seconds = 0;
};
[[nodiscard]] bool read_online_chunks(const OnlineAudioSource& source, AudioChunkConsumer& consumer,
    Failure& failure, const std::atomic<bool>* cancelled = nullptr, double from_seconds = 0.0);
[[nodiscard]] bool prepare_online_audio(const OnlineAudioSource& source, double duration,
    std::shared_ptr<const PreparedAudio>& prepared, Failure& failure, const std::atomic<bool>* cancelled = nullptr);
}
