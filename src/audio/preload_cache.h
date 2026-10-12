#pragma once
#include "audio/prepared_audio.h"
namespace musicboard {
[[nodiscard]] bool prepare_audio_cached(const std::filesystem::path& path, const AudioInfo& info,
    const std::filesystem::path& cache_root, std::shared_ptr<const PreparedAudio>& prepared,
    bool& cache_hit, std::string& cache_note, Failure& failure,
    const std::atomic<bool>* cancelled, const std::function<void(double)>& progress);
}
