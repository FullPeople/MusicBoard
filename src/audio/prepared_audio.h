#pragma once

#include <functional>
#include <memory>
#include "audio/decoder.h"

namespace musicboard {
struct OnlineAudioSource;
class OnlineAudioSession;

inline constexpr double preload_head_seconds = 2.0;

// Immutable shared data: short clips are complete, long clips retain their
// first two seconds. Each active long voice owns a bounded streaming queue.
struct PreparedAudio final {
    std::filesystem::path path;
    DecodedAudio head;
    std::vector<float> envelope;
    std::uint64_t total_frames = 0;
    bool complete = false;
    std::shared_ptr<const OnlineAudioSource> online;
    std::shared_ptr<OnlineAudioSession> online_session;
    [[nodiscard]] double duration() const noexcept {
        return static_cast<double>(total_frames) / head.format.sample_rate;
    }
};

[[nodiscard]] bool prepare_audio(const std::filesystem::path& path, const AudioInfo& info,
    std::shared_ptr<const PreparedAudio>& prepared, Failure& failure,
    const std::atomic<bool>* cancelled = nullptr,
    const std::function<void(double)>& progress = {});

} // namespace musicboard
