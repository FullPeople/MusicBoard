#include "audio/envelope_worker.h"
#include <algorithm>
#include "audio/waveform.h"

namespace musicboard {

EnvelopeWorker::~EnvelopeWorker() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        for (const auto& [key, token] : preview_tokens_) { static_cast<void>(key); *token = true; }
        for (const auto& [key, token] : audio_tokens_) { static_cast<void>(key); *token = true; }
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void EnvelopeWorker::start_if_needed() {
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
}

void EnvelopeWorker::request(std::size_t key, const std::filesystem::path& path) {
    request_urgent(key, path);
}

void EnvelopeWorker::request_urgent(std::size_t key, const std::filesystem::path& path) {
    {
        const std::lock_guard lock(mutex_);
        if (delivered_envelopes_.contains(key) || results_.contains(key) ||
            preview_tokens_.contains(key) || audio_tokens_.contains(key)) return;
        auto token = std::make_shared<std::atomic<bool>>(false);
        preview_tokens_[key] = token;
        previews_.push_back(Job{key, path, false, token});
        start_if_needed();
    }
    wake_.notify_one();
}

void EnvelopeWorker::request_audio(std::size_t key, const std::filesystem::path& path) {
    {
        const std::lock_guard lock(mutex_);
        if (audio_tokens_.contains(key)) return;
        // Playback provides an envelope. Cancel an in-flight preview as well as
        // queued previews; interactive callers can request previews again.
        for (const auto& [preview_key, token] : preview_tokens_) {
            static_cast<void>(preview_key);
            *token = true;
        }
        preview_tokens_.clear();
        previews_.clear();
        auto token = std::make_shared<std::atomic<bool>>(false);
        audio_tokens_[key] = token;
        playback_.push_back(Job{key, path, true, token});
        start_if_needed();
    }
    wake_.notify_one();
}

void EnvelopeWorker::cancel_audio_locked(std::size_t key) {
    if (const auto found = audio_tokens_.find(key); found != audio_tokens_.end()) {
        *found->second = true;
        audio_tokens_.erase(found);
    }
    std::erase_if(playback_, [key](const Job& job) { return job.key == key; });
    audio_results_.erase(key);
}

void EnvelopeWorker::cancel_audio(std::size_t key) {
    { const std::lock_guard lock(mutex_); cancel_audio_locked(key); }
    wake_.notify_one();
}

void EnvelopeWorker::forget(std::size_t key) {
    {
        const std::lock_guard lock(mutex_);
        cancel_audio_locked(key);
        if (const auto found = preview_tokens_.find(key); found != preview_tokens_.end()) {
            *found->second = true;
            preview_tokens_.erase(found);
        }
        std::erase_if(previews_, [key](const Job& job) { return job.key == key; });
        delivered_envelopes_.erase(key);
        results_.erase(key);
    }
    wake_.notify_one();
}

bool EnvelopeWorker::take(std::size_t key, std::vector<float>& envelope,
                           bool& success, Failure& failure) {
    const std::lock_guard lock(mutex_);
    const auto found = results_.find(key);
    if (found == results_.end()) return false;
    envelope = std::move(found->second.envelope);
    failure = std::move(found->second.failure);
    success = failure.empty();
    results_.erase(found);
    delivered_envelopes_.insert(key);
    return true;
}

bool EnvelopeWorker::take_audio(std::size_t key, DecodedAudio& audio,
                                 bool& success, Failure& failure) {
    {
        const std::lock_guard lock(mutex_);
        const auto found = audio_results_.find(key);
        if (found == audio_results_.end()) return false;
        audio = std::move(found->second.audio);
        failure = std::move(found->second.failure);
        success = failure.empty();
        audio_results_.erase(found);
        audio_tokens_.erase(key);
    }
    wake_.notify_one();
    return true;
}

std::size_t EnvelopeWorker::completed_count() const noexcept { return completed_.load(); }

void EnvelopeWorker::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] {
                return stopping_ || (audio_results_.empty() && !playback_.empty()) ||
                       (playback_.empty() && !previews_.empty());
            });
            if (stopping_) return;
            auto& queue = playback_.empty() ? previews_ : playback_;
            job = std::move(queue.front());
            queue.pop_front();
        }
        DecodedAudio audio;
        std::vector<float> envelope;
        Failure failure;
        Failure waveform_failure;
        if (job.audio) {
            if (decode_audio_file(job.path, audio, failure, job.cancelled.get())) {
                envelope = compute_waveform_envelope(audio, kDefaultWaveformBuckets, waveform_failure);
            }
        } else if (!extract_waveform_file(job.path, envelope, failure, job.cancelled.get())) {
            // Deliver the original file/backend failure to the UI.
        }
        {
            const std::lock_guard lock(mutex_);
            if (stopping_) return;
            if (job.cancelled->load()) continue;
            if (job.audio) {
                audio_results_[job.key] = AudioResult{std::move(audio), failure};
                if (failure.empty() && !delivered_envelopes_.contains(job.key)) {
                    results_[job.key] = Result{std::move(envelope), std::move(waveform_failure)};
                }
            } else {
                preview_tokens_.erase(job.key);
                results_[job.key] = Result{std::move(envelope), std::move(failure)};
            }
            completed_.fetch_add(1);
        }
    }
}

} // namespace musicboard
