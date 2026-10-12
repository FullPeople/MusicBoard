#pragma once

#include <xaudio2.h>
#include <cstdint>
#include <vector>
#include "core/failure.h"

namespace musicboard {

// Play the suffix once, then loop the whole original buffer. LoopBegin may
// precede PlayBegin; copying just the prefix would loop only part of the song.
[[nodiscard]] inline bool make_source_buffer(const std::vector<std::int16_t>& pcm,
    std::uint16_t channels, std::uint64_t first_frame, bool loop,
    XAUDIO2_BUFFER& buffer, Failure& failure) {
    buffer = XAUDIO2_BUFFER{};
    if (channels == 0 || pcm.empty() || pcm.size() % channels != 0 ||
        pcm.size() > XAUDIO2_MAX_BUFFER_BYTES / sizeof(std::int16_t) ||
        first_frame >= pcm.size() / channels) {
        failure.set(fail_code::invalid_argument, "Invalid PCM buffer: samples=" +
            std::to_string(pcm.size()) + " channels=" + std::to_string(channels) +
            " first_frame=" + std::to_string(first_frame));
        return false;
    }
    const auto total = static_cast<UINT32>(pcm.size() / channels);
    buffer.AudioBytes = static_cast<UINT32>(pcm.size() * sizeof(std::int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(pcm.data());
    buffer.PlayBegin = static_cast<UINT32>(first_frame);
    buffer.PlayLength = total - buffer.PlayBegin;
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (loop) {
        buffer.LoopBegin = 0;
        buffer.LoopLength = total;
        buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
    }
    return true;
}

} // namespace musicboard
