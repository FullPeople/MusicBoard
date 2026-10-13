#include "audio/stream_feed.h"
#include <algorithm>
#include "core/path_utf8.h"
#include "audio/online_audio.h"

namespace musicboard {
void StreamFeed::start(IXAudio2SourceVoice* voice, double from, bool loop) {
    voice_ = voice;
    staging_.reserve(static_cast<std::size_t>(audio_->head.format.sample_rate) * audio_->head.format.channels / 2);
    thread_ = std::thread([this, from, loop] { run(from, loop); });
}
void StreamFeed::cancel() {
    cancelled_ = true;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}
void StreamFeed::OnBufferEnd(void* context) {
    if (context) static_cast<Block*>(context)->free.store(true);
    wake_.notify_one();
}
void StreamFeed::OnVoiceError(void*, HRESULT error) { voice_error_ = error; wake_.notify_one(); }
bool StreamFeed::take_failure(Failure& failure) {
    const std::lock_guard lock(mutex_);
    if (failure_.empty()) return false;
    failure = std::move(failure_); failure_ = {};
    return true;
}
bool StreamFeed::submit(Failure& failure) {
    if (staging_.empty()) return true;
    Block* available = nullptr;
    {
        std::unique_lock lock(mutex_);
        // Completion is published without taking a lock on the audio thread.
        // A bounded wait also covers notification between predicate and sleep.
        while (!wake_.wait_for(lock, std::chrono::milliseconds(20), [this, &available] {
            if (cancelled_ || FAILED(voice_error_.load())) return true;
            for (auto& block : blocks_) if (block.free.load()) { available = &block; return true; }
            return false;
        })) {}
    }
    if (cancelled_) { failure.set("audio.cancelled", path_to_utf8(audio_->path) + " stream cancelled"); return false; }
    if (!available || FAILED(voice_error_.load())) {
        failure.set(fail_code::decode_failed, path_to_utf8(audio_->path) + " XAudio2 stream error=" + std::to_string(voice_error_.load()));
        return false;
    }
    available->samples.swap(staging_);
    staging_.clear();
    available->free = false;
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(available->samples.size() * sizeof(std::int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(available->samples.data());
    buffer.pContext = available;
    const auto hr = voice_->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
        available->free = true;
        failure.set(fail_code::decode_failed, path_to_utf8(audio_->path) + " SubmitSourceBuffer=" + std::to_string(hr));
        return false;
    }
    return true;
}
bool StreamFeed::consume(const AudioFormat& format, const std::int16_t* samples,
                         std::size_t frames, Failure& failure) {
    if (format.channels != audio_->head.format.channels || format.sample_rate != audio_->head.format.sample_rate) {
        failure.set(fail_code::decode_failed, path_to_utf8(audio_->path) + " streaming format changed"); return false;
    }
    const auto block_samples = static_cast<std::size_t>(format.sample_rate / 2) * format.channels;
    std::size_t count = frames * format.channels;
    while (count != 0) {
        const auto portion = std::min(count, block_samples - staging_.size());
        staging_.insert(staging_.end(), samples, samples + portion);
        samples += portion; count -= portion;
        if (staging_.size() == block_samples && !submit(failure)) return false;
    }
    return true;
}
void StreamFeed::run(double from, bool loop) {
    Failure failure;
    do {
        const bool read = audio_->online ? read_prepared_online_chunks(*audio_, *this, failure, &cancelled_, from)
                                        : read_audio_chunks(audio_->path, *this, failure, &cancelled_, from);
        if (!read || !submit(failure)) break;
        from = 0.0;
    } while (loop && !cancelled_);
    if (!failure.empty() && !cancelled_) {
        const std::lock_guard lock(mutex_); failure_ = std::move(failure);
    }
    finished_ = true;
}
} // namespace musicboard
