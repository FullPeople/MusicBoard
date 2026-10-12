#pragma once

#include <xaudio2.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/decoder.h"
#include "audio/stream_feed.h"
#include "audio/voice_timeline.h"
#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 音频引擎（XAudio2）
//
// 与 Godot 版的对应关系：
//
//   Godot 版                         这里
//   ─────────────────────────────    ──────────────────────────────────────
//   手搓 bgm_players / sfx_players    IXAudio2SourceVoice，DJ 按曲目分配独立槽位
//   AudioServer 总线                 两条 IXAudio2SubmixVoice（BGM / SFX）
//   finished 信号里重播 + 补淡入      XAUDIO2_LOOP_INFINITE，交给音频线程
//   每帧 delta 累加做淡入淡出         绝对时间戳 + VoiceTimeline（见 voice_timeline.h）
//
// 两条刻意的设计：
//
//   1. BGM 两槽、音效四台，加上每条 DJ 音效的独立槽位；超过上限明确失败。
//      导入扩容保留其他槽位，不能把正在播放的音效挤掉。
//   2. PCM 由槽位或共享预载对象持有。换源/停止时先取消并等待流式线程，
//      再 DestroyVoice，最后释放缓冲，避免 XAudio2 读到已释放的数据。
// ============================================================================

enum class AudioBus : std::uint8_t {
    bgm = 0,
    sfx = 1,
};

[[nodiscard]] const char* audio_bus_name(AudioBus bus) noexcept;

// 槽位状态，给界面用
struct VoiceSlotState final {
    bool in_use = false;
    bool playing = false;      // 正在出声（未暂停、未淡出到 0）
    bool paused = false;
    bool stopping = false;
    bool looping = false;
    double position_seconds = 0.0;
    double duration_seconds = 0.0;
    float current_gain = 0.0f;
    std::string name;          // 曲名，仅供日志与调试
};

class AudioEngine final {
public:
    // BGM 两个：交叉淡入淡出需要在旧曲淡出的同时新曲淡入
    static constexpr std::size_t bgm_slot_count = 2;
    // 音效四台，与设计稿一致
    static constexpr std::size_t sfx_slot_count = 4;

    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // 建立 XAudio2、主声部与两条总线。没有音频输出设备时会失败并说明原因。
    [[nodiscard]] bool initialize(Failure& failure);
    void shutdown() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] std::uint32_t output_sample_rate() const noexcept { return mastering_rate_; }
    [[nodiscard]] std::uint32_t output_channels() const noexcept { return mastering_channels_; }
    [[nodiscard]] const char* backend_name() const noexcept { return "xaudio2"; }

    // 播放。pcm 会被拷进槽位，调用方不必保持 referenced 数据存活。
    // fade_in_seconds <= 0 表示立即满音量。
    [[nodiscard]] bool play(AudioBus bus, std::size_t slot, const DecodedAudio& audio,
                            std::string name, double fade_in_seconds, bool loop, Failure& failure);

    [[nodiscard]] bool play_prepared(AudioBus bus, std::size_t slot, std::shared_ptr<const PreparedAudio> audio,
                                    std::string name, double fade, bool loop, Failure& failure);
    [[nodiscard]] bool configure_sfx_slots(std::size_t count, Failure& failure);
    [[nodiscard]] bool ensure_sfx_slots(std::size_t count, Failure& failure);
    [[nodiscard]] std::size_t sfx_capacity() const noexcept { return sfx_slots_.size(); }
    [[nodiscard]] bool take_failure(Failure& failure);

    // 淡出停止。fade_out <= 0 表示立即停止。
    [[nodiscard]] bool stop(AudioBus bus, std::size_t slot, double fade_out_seconds, Failure& failure);

    [[nodiscard]] bool pause(AudioBus bus, std::size_t slot, Failure& failure, double fade = 0.0);
    [[nodiscard]] bool resume(AudioBus bus, std::size_t slot, Failure& failure, double fade = 0.0);
    // Incoming has been prepared successfully. Crossfade overlaps; sequential waits for outgoing.
    [[nodiscard]] bool transition_bgm(std::size_t outgoing, std::size_t incoming, double seconds,
                                     bool overlap, Failure& failure);
    [[nodiscard]] bool set_end_fade(AudioBus bus, std::size_t slot, double seconds, Failure& failure);
    [[nodiscard]] bool seek(AudioBus bus, std::size_t slot, double seconds, Failure& failure);

    // 运行时改循环。**正在播的那一条也要立即改** ——
    // 只影响"下一次播放"的开关，在界面上就等于"按钮关掉了声音还在循环"。
    [[nodiscard]] bool set_loop(AudioBus bus, std::size_t slot, bool loop, Failure& failure);
    // 总线音量（两条完全独立）
    [[nodiscard]] bool set_bus_volume(AudioBus bus, float linear_gain, Failure& failure);
    [[nodiscard]] float bus_volume(AudioBus bus) const noexcept;

    // 单台音量（音效台各自独立）
    [[nodiscard]] bool set_slot_volume(AudioBus bus, std::size_t slot, float linear_gain,
                                       Failure& failure);

    // 推进淡入淡出并把完成的声部回收。now 必须是单调递增的秒数。
    void update(double now_seconds);

    [[nodiscard]] VoiceSlotState slot_state(AudioBus bus, std::size_t slot) const noexcept;
    [[nodiscard]] std::size_t active_count(AudioBus bus) const noexcept;

    // 找一个空闲槽位；满了返回 slot_count（调用方据此明确失败，而不是悄悄覆盖别人）。
    [[nodiscard]] std::size_t find_free_slot(AudioBus bus) const noexcept;

private:
    // 一个声部槽位
    struct Slot final {
        // 裸指针而不是 ComPtr：XAudio2 的 voice 接口**不是 COM 引用计数的**，
        // 它们没有 Release()，必须用 DestroyVoice() 销毁。
        // 用 ComPtr 会编译不过（wrl/client.h 找不到 Release 成员）。
        IXAudio2SourceVoice* voice = nullptr;
        IXAudio2Voice* output_target = nullptr;
        std::vector<std::int16_t> pcm;            // 正在被 XAudio2 读取
        std::shared_ptr<const PreparedAudio> prepared;
        std::unique_ptr<StreamFeed> stream;
        [[nodiscard]] const std::vector<std::int16_t>& samples() const noexcept {
            return prepared ? prepared->head.samples : pcm;
        }
        VoiceTimeline timeline;
        std::uint32_t sample_rate = 44100;
        std::uint16_t channels = 2;
        bool in_use = false;
        bool paused = false;
        bool pause_pending = false;
        bool delayed_start = false;
        double start_after = 0.0;
        double end_fade_seconds = 0.0;
        bool natural_fade = false;
        bool looping = false;
        float slot_volume = 1.0f;
        std::string name;

        // 淡入在下一次 update(now) 开始。停止暂停或尚未开始的声部立即生效。
        bool pending_start = false;
        double pending_fade_in = 0.0;
        bool pending_stop = false;

        double last_update_time = 0.0;
        // seek 之后位置 = seek 目标 + (当前 SamplesPlayed - seek 时的 SamplesPlayed) / 速率。
        // 注意 SamplesPlayed **不会**被 FlushSourceBuffers 重置，实测确认过 ——
        // 所以不能简单地"加一个固定偏移"，那样会越报越大。
        double seek_offset_seconds = 0.0;
        std::uint64_t samples_played_at_seek = 0;

        [[nodiscard]] double duration_seconds() const noexcept {
            if (prepared) return prepared->duration();
            if (channels == 0 || sample_rate == 0) {
                return 0.0;
            }
            return static_cast<double>(pcm.size() / channels) / static_cast<double>(sample_rate);
        }
    };

    [[nodiscard]] bool create_slot_voice(Slot& slot, const AudioFormat& format, Failure& failure);
    void hard_stop(Slot& slot) noexcept;

    [[nodiscard]] Slot& slot_at(AudioBus bus, std::size_t index) noexcept;
    [[nodiscard]] const Slot& slot_at(AudioBus bus, std::size_t index) const noexcept;
    [[nodiscard]] bool valid_slot(AudioBus bus, std::size_t index, Failure& failure) const noexcept;

    [[nodiscard]] std::size_t slot_count(AudioBus bus) const noexcept {
        return bus == AudioBus::bgm ? bgm_slot_count : sfx_slots_.size();
    }

    Microsoft::WRL::ComPtr<IXAudio2> engine_;
    IXAudio2MasteringVoice* mastering_voice_ = nullptr;
    IXAudio2SubmixVoice* bgm_bus_voice_ = nullptr;
    IXAudio2SubmixVoice* sfx_bus_voice_ = nullptr;

    std::array<Slot, bgm_slot_count> bgm_slots_{};
    std::vector<Slot> sfx_slots_{sfx_slot_count};
    std::vector<Failure> failures_;

    float bgm_bus_volume_ = 1.0f;
    float sfx_bus_volume_ = 1.0f;

    std::uint32_t mastering_rate_ = 0;
    std::uint32_t mastering_channels_ = 0;
    bool initialized_ = false;
    double now_ = 0.0;
    // 从 seconds 处重新提交缓冲（循环时切成两段排进队列，见 .cpp 里的说明）。
    // 声明放在 Slot 之后 —— 参数用到 Slot，写在前面编译器认不出这个名字。
    [[nodiscard]] bool submit_from(Slot& slot, double seconds, Failure& failure);
};

}  // namespace musicboard
