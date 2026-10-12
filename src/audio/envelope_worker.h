#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "audio/decoder.h"

namespace musicboard {

// Playback is independent of previews. Cancelled generations cannot deliver
// into newer requests. Only one completed PCM result waits for the UI at a time.
class EnvelopeWorker final {
public:
    EnvelopeWorker() = default;
    ~EnvelopeWorker();
    EnvelopeWorker(const EnvelopeWorker&) = delete;
    EnvelopeWorker& operator=(const EnvelopeWorker&) = delete;
    void request(std::size_t key, const std::filesystem::path& path);
    void request_urgent(std::size_t key, const std::filesystem::path& path);
    void request_audio(std::size_t key, const std::filesystem::path& path);
    void cancel_audio(std::size_t key);
    void forget(std::size_t key);
    // bool is readiness; Failure carries a completed operation's error.
    [[nodiscard]] bool take(std::size_t key, std::vector<float>& envelope,
                            bool& success, Failure& failure);
    [[nodiscard]] bool take_audio(std::size_t key, DecodedAudio& audio,
                                  bool& success, Failure& failure);
    [[nodiscard]] std::size_t completed_count() const noexcept;

private:
    using Cancellation = std::shared_ptr<std::atomic<bool>>;
    struct Job final {
        std::size_t key = 0;
        std::filesystem::path path;
        bool audio = false;
        Cancellation cancelled;
    };
    struct Result final { std::vector<float> envelope; Failure failure; };
    struct AudioResult final { DecodedAudio audio; Failure failure; };
    void run();
    void start_if_needed();
    void cancel_audio_locked(std::size_t key);
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> previews_;
    std::deque<Job> playback_;
    std::unordered_map<std::size_t, Cancellation> preview_tokens_;
    std::unordered_map<std::size_t, Cancellation> audio_tokens_;
    std::unordered_set<std::size_t> delivered_envelopes_;
    std::unordered_map<std::size_t, Result> results_;
    std::unordered_map<std::size_t, AudioResult> audio_results_;
    std::thread thread_;
    bool stopping_ = false;
    std::atomic<std::size_t> completed_{0};
};

} // namespace musicboard
