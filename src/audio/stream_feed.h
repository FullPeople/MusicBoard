#pragma once
#include <xaudio2.h>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "audio/prepared_audio.h"

namespace musicboard {
// The producer owns a fixed pool. Audio callbacks only publish completion;
// cancellation joins the producer before its voice and buffers are destroyed.
class StreamFeed final : public IXAudio2VoiceCallback, private AudioChunkConsumer {
public:
    explicit StreamFeed(std::shared_ptr<const PreparedAudio> audio) : audio_(std::move(audio)) {}
    ~StreamFeed() override { cancel(); }
    void start(IXAudio2SourceVoice* voice, double from, bool loop);
    void cancel();
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] bool take_failure(Failure& failure);
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) override;
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override;
private:
    struct Block { std::vector<std::int16_t> samples; std::atomic<bool> free{true}; };
    [[nodiscard]] bool consume(const AudioFormat& format, const std::int16_t* samples,
                               std::size_t frames, Failure& failure) override;
    [[nodiscard]] bool submit(Failure& failure);
    void run(double from, bool loop);
    std::shared_ptr<const PreparedAudio> audio_;
    IXAudio2SourceVoice* voice_ = nullptr;
    std::array<Block, 8> blocks_;
    std::vector<std::int16_t> staging_;
    std::thread thread_;
    std::atomic<bool> cancelled_{false}, finished_{false};
    std::atomic<HRESULT> voice_error_{S_OK};
    std::mutex mutex_;
    std::condition_variable wake_;
    Failure failure_;
};
} // namespace musicboard
