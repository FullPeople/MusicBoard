#include "audio/audio_engine.h"
#include "audio/source_buffer.h"

#include <algorithm>
#include <cmath>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace musicboard {
namespace {

[[nodiscard]] std::string hresult_hex(HRESULT hr) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

// 一个宽字符的显示名转成 UTF-8，给日志用
[[nodiscard]] std::string narrow_ascii(const wchar_t* text) {
    std::string out;
    if (text == nullptr) {
        return out;
    }
    for (const wchar_t ch : std::wstring_view(text)) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

}  // namespace

const char* audio_bus_name(AudioBus bus) noexcept {
    return bus == AudioBus::bgm ? "bgm" : "sfx";
}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize(Failure& failure) {
    if (initialized_) {
        return true;
    }

    // XAudio2 内部会用到 COM。MULTITHREADED 让它可以在别的线程上被调用。
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        failure.set(fail_code::backend_unavailable, "CoInitializeEx 失败 " + hresult_hex(com));
        return false;
    }

    HRESULT hr = ::XAudio2Create(engine_.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !engine_) {
        failure.set(fail_code::backend_unavailable,
                    "XAudio2Create 失败 " + hresult_hex(hr) +
                        "。可能是系统缺少 XAudio2 运行时。");
        return false;
    }

    hr = engine_->CreateMasteringVoice(&mastering_voice_);
    if (FAILED(hr) || mastering_voice_ == nullptr) {
        // 这一条要给出明确结论 —— 没有输出设备时用户会看到"点了没声音"，
        // 而真正的原因在这里。
        failure.set(fail_code::backend_unavailable,
                    "创建主声部失败 " + hresult_hex(hr) +
                        "。通常表示这台机器没有可用的音频输出设备（虚拟机、远程桌面、无声卡）。");
        engine_.Reset();
        return false;
    }

    XAUDIO2_VOICE_DETAILS master_details{};
    mastering_voice_->GetVoiceDetails(&master_details);
    mastering_rate_ = master_details.InputSampleRate;
    mastering_channels_ = master_details.InputChannels;

    // 两条总线。输入格式必须与主声部一致 —— 它们是混音后的目标。
    hr = engine_->CreateSubmixVoice(&bgm_bus_voice_, mastering_channels_, mastering_rate_, 0, 0,
                                    nullptr, nullptr);
    if (FAILED(hr) || bgm_bus_voice_ == nullptr) {
        failure.set(fail_code::backend_unavailable, "创建 BGM 总线失败 " + hresult_hex(hr));
        shutdown();
        return false;
    }
    hr = engine_->CreateSubmixVoice(&sfx_bus_voice_, mastering_channels_, mastering_rate_, 0, 0,
                                    nullptr, nullptr);
    if (FAILED(hr) || sfx_bus_voice_ == nullptr) {
        failure.set(fail_code::backend_unavailable, "创建 SFX 总线失败 " + hresult_hex(hr));
        shutdown();
        return false;
    }

    bgm_bus_voice_->SetVolume(bgm_bus_volume_);
    sfx_bus_voice_->SetVolume(sfx_bus_volume_);

    initialized_ = true;
    return true;
}

void AudioEngine::shutdown() noexcept {
    // 两个数组长度不同（2 和 4），不能用同一个 initializer_list 遍历 —— 用泛型 lambda。
    const auto release_all = [this](auto& collection) {
        for (Slot& slot : collection) hard_stop(slot);
    };
    release_all(bgm_slots_);
    release_all(sfx_slots_);

    if (bgm_bus_voice_ != nullptr) {
        bgm_bus_voice_->DestroyVoice();
        bgm_bus_voice_ = nullptr;
    }
    if (sfx_bus_voice_ != nullptr) {
        sfx_bus_voice_->DestroyVoice();
        sfx_bus_voice_ = nullptr;
    }
    if (mastering_voice_ != nullptr) {
        mastering_voice_->DestroyVoice();
        mastering_voice_ = nullptr;
    }
    engine_.Reset();
    initialized_ = false;
}

AudioEngine::Slot& AudioEngine::slot_at(AudioBus bus, std::size_t index) noexcept {
    return bus == AudioBus::bgm ? bgm_slots_[index] : sfx_slots_[index];
}

const AudioEngine::Slot& AudioEngine::slot_at(AudioBus bus, std::size_t index) const noexcept {
    return bus == AudioBus::bgm ? bgm_slots_[index] : sfx_slots_[index];
}

bool AudioEngine::valid_slot(AudioBus bus, std::size_t index, Failure& failure) const noexcept {
    if (index >= slot_count(bus)) {
        failure.set(fail_code::invalid_argument,
                    std::string("槽位下标越界: ") + audio_bus_name(bus) + "[" + std::to_string(index) +
                        "]，上限 " + std::to_string(slot_count(bus)));
        return false;
    }
    return true;
}

bool AudioEngine::create_slot_voice(Slot& slot, const AudioFormat& format, Failure& failure) {
    // 已经建过就直接复用 —— 声部数量固定，不需要每次换曲都重建。
    if (slot.voice) {
        return true;
    }
    if (format.sample_rate == 0 || format.channels == 0 || format.bits_per_sample != 16) {
        failure.set(fail_code::invalid_argument,
                    "只接受 16 位 PCM，收到 " + std::to_string(format.sample_rate) + "Hz/" +
                        std::to_string(format.channels) + "ch/" +
                        std::to_string(format.bits_per_sample) + "bit");
        return false;
    }

    WAVEFORMATEX wave_format{};
    wave_format.wFormatTag = WAVE_FORMAT_PCM;
    wave_format.nChannels = format.channels;
    wave_format.nSamplesPerSec = format.sample_rate;
    wave_format.wBitsPerSample = format.bits_per_sample;
    wave_format.nBlockAlign = static_cast<WORD>(format.channels * format.bits_per_sample / 8);
    wave_format.nAvgBytesPerSec = wave_format.nSamplesPerSec * wave_format.nBlockAlign;
    wave_format.cbSize = 0;

    // 源声部的格式可以与主声部不同，XAudio2 会自己重采样
    // （本机主声部是 48000，而曲库是 44100 —— 这件事由引擎处理，上层不用管）。
    IXAudio2SourceVoice* voice = nullptr;
    const HRESULT hr = engine_->CreateSourceVoice(&voice, &wave_format, 0, XAUDIO2_DEFAULT_FREQ_RATIO,
                                                 slot.stream.get(), nullptr, nullptr);
    if (FAILED(hr) || voice == nullptr) {
        failure.set(fail_code::backend_unavailable,
                    "创建源声部失败 " + hresult_hex(hr) + "（格式 " +
                        std::to_string(format.sample_rate) + "Hz/" + std::to_string(format.channels) +
                        "ch）");
        return false;
    }
    slot.voice = voice;
    slot.sample_rate = format.sample_rate;
    slot.channels = format.channels;

    // 声部输出到对应总线，这样音量控制是"总线一次、单台又一次"，互不干扰
    return true;
}

bool AudioEngine::play(AudioBus bus, std::size_t slot_index, const DecodedAudio& audio,
                       std::string name, double fade_in_seconds, bool loop, Failure& failure) {
    if (!initialized_) {
        failure.set(fail_code::backend_unavailable, "音频引擎尚未初始化");
        return false;
    }
    if (!valid_slot(bus, slot_index, failure)) {
        return false;
    }
    if (audio.samples.empty()) {
        failure.set(fail_code::invalid_argument, "要播放的音频没有样本");
        return false;
    }

    XAUDIO2_BUFFER validated{};
    if (!make_source_buffer(audio.samples, audio.format.channels, 0, loop, validated, failure)) return false;
    if (audio.format.sample_rate < XAUDIO2_MIN_SAMPLE_RATE ||
        audio.format.sample_rate > XAUDIO2_MAX_SAMPLE_RATE ||
        audio.format.channels > XAUDIO2_MAX_AUDIO_CHANNELS || audio.format.bits_per_sample != 16 ||
        !std::isfinite(fade_in_seconds)) {
        failure.set(fail_code::invalid_argument, "Invalid playback format/fade: " +
            std::to_string(audio.format.sample_rate) + "Hz/" + std::to_string(audio.format.channels) + "ch");
        return false;
    }
    Slot& slot = slot_at(bus, slot_index);
    hard_stop(slot);
    if (!create_slot_voice(slot, audio.format, failure)) return false;
    slot.pcm = audio.samples;
    slot.name = std::move(name);
    slot.in_use = true;
    slot.paused = false;
    slot.looping = loop;

    // 源声部输出到总线
    IXAudio2SubmixVoice* target = bus == AudioBus::bgm ? bgm_bus_voice_ : sfx_bus_voice_;
    const HRESULT route_hr = slot.voice->SetOutputVoices(nullptr);   // 先解除，避免重复连接
    if (FAILED(route_hr)) {
        failure.set(fail_code::platform_api_failed, "SetOutputVoices 失败 " + hresult_hex(route_hr));
        return false;
    }
    XAUDIO2_SEND_DESCRIPTOR send{};
    send.Flags = 0;
    send.pOutputVoice = target;
    XAUDIO2_VOICE_SENDS sends{};
    sends.SendCount = 1;
    sends.pSends = &send;
    const HRESULT send_hr = slot.voice->SetOutputVoices(&sends);
    if (FAILED(send_hr)) {
        failure.set(fail_code::platform_api_failed,
                    "把声部接到总线上失败 " + hresult_hex(send_hr));
        return false;
    }

    // 提交缓冲。pcm 会被 XAudio2 直接读取，所以它必须活到声部停止为止 ——
    // 这正是 Slot 持有 pcm 而不是外部持有的原因。
    XAUDIO2_BUFFER buffer{};
    if (!make_source_buffer(slot.samples(), slot.channels, 0, loop, buffer, failure)) {
        hard_stop(slot);
        return false;
    }

    HRESULT hr = slot.voice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "SubmitSourceBuffer 失败 " + hresult_hex(hr));
        hard_stop(slot);
        return false;
    }

    // 淡入的起点增益由时间线算，这里先把声部设成起始值再启动，
    // 避免出现"先满音量响一下再降下来"。
    const float initial_gain = fade_in_seconds > 0.0 ? 0.0f : 1.0f;
    slot.voice->SetVolume(initial_gain * slot.slot_volume);
    hr = slot.voice->Start(0);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "Start 失败 " + hresult_hex(hr));
        hard_stop(slot);
        return false;
    }

    // **位置基准必须在这里重置。**
    //
    // 位置 = (SamplesPlayed − samples_played_at_seek) / 采样率 + seek_offset_seconds。
    // 这两个字段原来只在 seek 里写，于是换曲之后它们还留着上一首的值：
    // 新曲的进度从那点开始算，残留比新曲还长时就直接是"满进度"。
    // （用户的"大部分音波图进度不对/保持满进度"就是它。）
    //
    // 而且 SamplesPlayed 是**声部一生**的累计值，Flush 不会清零，
    // 所以基线取"Start 之后当前的值"，不能想当然地写 0。
    {
        XAUDIO2_VOICE_STATE baseline{};
        slot.voice->GetState(&baseline, 0);
        slot.samples_played_at_seek = baseline.SamplesPlayed;
        slot.seek_offset_seconds = 0.0;
    }

    // play() 拿不到调用方的时钟，所以只挂起请求；
    // 下一次 update(now) 会用同一个时钟源落实起点，保证时间线不混用两个时钟。
    slot.pending_start = true;
    slot.pending_fade_in = fade_in_seconds;
    slot.end_fade_seconds = fade_in_seconds;
    slot.pending_stop = false;
    return true;
}

bool AudioEngine::stop(AudioBus bus, std::size_t slot_index, double fade_out_seconds,
                       Failure& failure) {
    if (!valid_slot(bus, slot_index, failure)) {
        return false;
    }
    Slot& slot = slot_at(bus, slot_index);
    if (!slot.in_use) {
        // 已经是空的：不算失败，但也不假装做了什么
        return true;
    }
    if (!std::isfinite(fade_out_seconds)) {
        failure.set(fail_code::invalid_argument, "Stop fade must be finite");
        return false;
    }
    if (fade_out_seconds <= 0.0 || slot.paused || slot.pending_start) {
        hard_stop(slot);
        return true;
    }
    slot.pause_pending = false;
    if (slot.timeline.stopping) {
        const double remaining = std::max(0.0, slot.timeline.fade_out_started_at + slot.timeline.fade_out_seconds - now_);
        if (fade_out_seconds < remaining) {
            const float current = slot.timeline.gain_at(now_);
            slot.timeline.begin_from(now_, 0, current, current);
        }
    }
    slot.timeline.begin_stop(slot.last_update_time, fade_out_seconds);
    slot.pending_stop = true;

    return true;
}

bool AudioEngine::configure_sfx_slots(std::size_t count, Failure& failure) {
    if (count < sfx_slot_count || count > 4096) {
        failure.set(fail_code::invalid_argument, "SFX slot count=" + std::to_string(count) + " allowed 4..4096");
        return false;
    }
    for (auto& slot : sfx_slots_) hard_stop(slot);
    sfx_slots_.resize(count);
    return true;
}

bool AudioEngine::ensure_sfx_slots(std::size_t count, Failure& failure) {
    if (count <= sfx_slots_.size()) return true;
    if (count > 4096) {
        failure.set(fail_code::invalid_argument, "SFX slot count=" + std::to_string(count) + " exceeds 4096");
        return false;
    }
    sfx_slots_.resize(count);
    return true;
}

bool AudioEngine::take_failure(Failure& failure) {
    if (failures_.empty()) return false;
    failure = std::move(failures_.front()); failures_.erase(failures_.begin());
    return true;
}

bool AudioEngine::play_prepared(AudioBus bus, std::size_t index,
    std::shared_ptr<const PreparedAudio> audio, std::string name, double fade, bool loop, Failure& failure) {
    if (!initialized_) { failure.set(fail_code::backend_unavailable, "Prepared playback engine not initialized"); return false; }
    if (!valid_slot(bus, index, failure)) return false;
    if (!audio || audio->head.samples.empty() || !std::isfinite(fade)) {
        failure.set(fail_code::invalid_argument, "Prepared playback has no data: " + name); return false;
    }
    XAUDIO2_BUFFER buffer{};
    if (!make_source_buffer(audio->head.samples, audio->head.format.channels, 0,
                            loop && audio->complete, buffer, failure)) return false;
    Slot& slot = slot_at(bus, index);
    hard_stop(slot);
    slot.prepared = std::move(audio);
    if (!slot.prepared->complete) {
        slot.stream = std::make_unique<StreamFeed>(slot.prepared);
        buffer.Flags = 0;
    }
    if (!create_slot_voice(slot, slot.prepared->head.format, failure)) { hard_stop(slot); return false; }
    slot.output_target = bus == AudioBus::bgm ? bgm_bus_voice_ : sfx_bus_voice_;
    XAUDIO2_SEND_DESCRIPTOR send{0, slot.output_target};
    XAUDIO2_VOICE_SENDS sends{1, &send};
    HRESULT hr = slot.voice->SetOutputVoices(&sends);
    if (SUCCEEDED(hr)) hr = slot.voice->SubmitSourceBuffer(&buffer);
    if (SUCCEEDED(hr)) hr = slot.voice->SetVolume((fade > 0.0 ? 0.0f : 1.0f) * slot.slot_volume);
    if (SUCCEEDED(hr)) hr = slot.voice->Start(0);
    if (FAILED(hr)) { failure.set(fail_code::platform_api_failed, "Prepared play " + name + " " + hresult_hex(hr)); hard_stop(slot); return false; }
    slot.name = std::move(name);
    slot.in_use = true;
    slot.looping = loop;
    slot.pending_start = true;
    slot.pending_fade_in = fade;
    slot.end_fade_seconds = fade;
    // Fresh voice starts at sample zero. Reading after Start can hide initial samples.
    slot.samples_played_at_seek = 0;
    if (slot.stream) slot.stream->start(slot.voice, slot.prepared->head.duration_seconds, loop);
    return true;
}

bool AudioEngine::pause(AudioBus bus, std::size_t slot_index, Failure& failure, double fade) {
    if (!valid_slot(bus, slot_index, failure)) return false;
    if (!std::isfinite(fade) || fade < 0) {
        failure.set(fail_code::invalid_argument, "Pause fade seconds=" + std::to_string(fade)); return false;
    }
    Slot& slot = slot_at(bus, slot_index);
    if (!slot.in_use || slot.paused || slot.pause_pending) return true;
    if (fade > 0 && !slot.pending_start && !slot.delayed_start) {
        const float gain = slot.timeline.gain_at(now_);
        slot.timeline.begin_from(now_, 0, gain, gain);
        slot.timeline.begin_stop(now_, fade);
        slot.pause_pending = true;
        return true;
    }
    const HRESULT hr = slot.voice->Stop(0);
    if (FAILED(hr)) { failure.set(fail_code::platform_api_failed, "Pause Stop " + hresult_hex(hr)); return false; }
    slot.paused = true;
    slot.delayed_start = false;
    slot.pending_start = false;
    slot.timeline.begin_from(now_, 0, 0, 0);
    return true;
}

bool AudioEngine::resume(AudioBus bus, std::size_t slot_index, Failure& failure, double fade) {
    if (!valid_slot(bus, slot_index, failure)) return false;
    if (!std::isfinite(fade) || fade < 0) {
        failure.set(fail_code::invalid_argument, "Resume fade seconds=" + std::to_string(fade)); return false;
    }
    Slot& slot = slot_at(bus, slot_index);
    if (!slot.in_use || (!slot.paused && !slot.pause_pending)) return true;
    const float from = slot.paused ? 0.0f : slot.timeline.gain_at(now_);
    HRESULT hr = slot.voice->SetVolume((fade > 0 ? from : 1.0f) * slot.slot_volume);
    if (SUCCEEDED(hr) && slot.paused) hr = slot.voice->Start(0);
    if (FAILED(hr)) { failure.set(fail_code::platform_api_failed, "Resume Start " + hresult_hex(hr)); return false; }
    slot.timeline.begin_from(now_, fade, from, 1.0f);
    slot.last_update_time = now_;
    slot.paused = false; slot.pause_pending = false; slot.pending_start = false;
    slot.pending_stop = false; slot.natural_fade = false;
    return true;
}

bool AudioEngine::set_end_fade(AudioBus bus, std::size_t index, double seconds, Failure& failure) {
    if (!valid_slot(bus, index, failure)) return false;
    if (!std::isfinite(seconds) || seconds < 0) {
        failure.set(fail_code::invalid_argument, "End fade seconds=" + std::to_string(seconds)); return false;
    }
    auto& slot = slot_at(bus, index);
    slot.end_fade_seconds = seconds;
    if (seconds == 0 && slot.natural_fade && !slot.pause_pending && !slot.pending_stop) {
        slot.timeline.begin_from(now_, 0.06, slot.timeline.gain_at(now_), 1.0f);
        slot.natural_fade = false;
    }
    return true;
}

bool AudioEngine::transition_bgm(std::size_t outgoing, std::size_t incoming, double seconds,
                                  bool overlap, Failure& failure) {
    if (!valid_slot(AudioBus::bgm, outgoing, failure) || !valid_slot(AudioBus::bgm, incoming, failure)) return false;
    if (outgoing == incoming || !std::isfinite(seconds) || seconds < 0) {
        failure.set(fail_code::invalid_argument, "BGM transition from=" + std::to_string(outgoing) +
            " to=" + std::to_string(incoming) + " seconds=" + std::to_string(seconds)); return false;
    }
    auto& old = bgm_slots_[outgoing]; auto& next = bgm_slots_[incoming];
    if (!next.in_use) { failure.set(fail_code::invalid_argument, "BGM incoming slot is empty"); return false; }
    if (!old.in_use || old.paused || old.pause_pending || old.pending_start || old.delayed_start || seconds == 0) { hard_stop(old); return true; }
    HRESULT hr = next.voice->SetVolume(0);
    if (SUCCEEDED(hr) && !overlap) hr = next.voice->Stop(0);
    if (FAILED(hr)) { failure.set(fail_code::platform_api_failed, "BGM transition " + hresult_hex(hr)); return false; }
    // Freeze the current outgoing level before replacing a previous transition.
    old.timeline.begin_from(now_, 0, old.timeline.gain_at(now_), old.timeline.gain_at(now_));
    old.timeline.begin_stop(now_, seconds, overlap ? FadeCurve::equal_power_out : FadeCurve::linear);
    old.pending_stop = true;
    if (overlap) {
        next.timeline.begin_from(now_, seconds, 0, 1, FadeCurve::equal_power_in);
        next.pending_start = false;
    } else {
        next.delayed_start = true;
        next.start_after = now_ + seconds;
    }
    return true;
}

// Keep the entire PCM region when seeking, including on subsequent loops.
bool AudioEngine::submit_from(Slot& slot, double seconds, Failure& failure) {
    if (slot.prepared && !slot.prepared->complete) {
        const auto prepared = slot.prepared;
        const bool paused = slot.paused;
        const bool loop = slot.looping;
        const auto name = slot.name;
        const auto timeline = slot.timeline;
        const double stamp = slot.last_update_time;
        // Stop/join before releasing old buffers. A replacement feed owns its callbacks.
        if (slot.stream) slot.stream->cancel();
        slot.voice->DestroyVoice(); slot.voice = nullptr;
        slot.stream = std::make_unique<StreamFeed>(prepared);
        if (!create_slot_voice(slot, prepared->head.format, failure)) { hard_stop(slot); return false; }
        XAUDIO2_SEND_DESCRIPTOR send{0, slot.output_target};
        XAUDIO2_VOICE_SENDS sends{1, &send};
        auto route = slot.voice->SetOutputVoices(&sends);
        if (SUCCEEDED(route)) route = slot.voice->SetVolume(timeline.gain_at(stamp) * slot.slot_volume);
        if (FAILED(route)) { failure.set(fail_code::platform_api_failed, "Stream seek route " + hresult_hex(route)); hard_stop(slot); return false; }
        slot.timeline = timeline; slot.name = name; slot.last_update_time = stamp;
        slot.seek_offset_seconds = seconds; slot.samples_played_at_seek = 0;
        const auto hr = slot.voice->Start(0);
        if (FAILED(hr)) { failure.set(fail_code::platform_api_failed, "Stream seek Start " + hresult_hex(hr)); hard_stop(slot); return false; }
        if (paused) slot.voice->Stop(0);
        slot.stream->start(slot.voice, seconds, loop);
        return true;
    }
    XAUDIO2_BUFFER buffer{};
    const auto first_frame = static_cast<std::uint64_t>(seconds * slot.sample_rate);
    if (!make_source_buffer(slot.samples(), slot.channels, first_frame, slot.looping, buffer, failure)) return false;
    HRESULT hr = slot.voice->Stop(0);
    if (SUCCEEDED(hr)) hr = slot.voice->FlushSourceBuffers();
    if (SUCCEEDED(hr)) hr = slot.voice->SubmitSourceBuffer(&buffer);
    if (SUCCEEDED(hr) && !slot.paused) hr = slot.voice->Start(0);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "Seek buffer submission " + hresult_hex(hr));
        hard_stop(slot);
        return false;
    }
    XAUDIO2_VOICE_STATE after{};
    slot.voice->GetState(&after, 0);
    slot.samples_played_at_seek = after.SamplesPlayed;
    slot.seek_offset_seconds = seconds;
    return true;
}

bool AudioEngine::set_loop(AudioBus bus, std::size_t slot_index, bool loop, Failure& failure) {
    if (!initialized_) {
        failure.set(fail_code::backend_unavailable, "音频引擎还没初始化");
        return false;
    }
    if (!valid_slot(bus, slot_index, failure)) return false;
    Slot& slot = slot_at(bus, slot_index);
    if (slot.looping == loop) return true;
    // Read position under the OLD loop mode (non-looping positions must not be
    // wrapped before the snapshot has been taken).
    const double position = slot_state(bus, slot_index).position_seconds;
    slot.looping = loop;
    if (!slot.in_use || slot.voice == nullptr) return true;
    return seek(bus, slot_index, position, failure);
}

bool AudioEngine::seek(AudioBus bus, std::size_t slot_index, double seconds, Failure& failure) {
    if (!valid_slot(bus, slot_index, failure)) {
        return false;
    }
    Slot& slot = slot_at(bus, slot_index);
    if (!slot.in_use || slot.voice == nullptr) {
        return true;
    }
    const double duration = slot.duration_seconds();
    if (duration <= 0.0) {
        return true;
    }
    if (!std::isfinite(seconds)) {
        failure.set(fail_code::invalid_argument, "Seek seconds must be finite");
        return false;
    }
    const double clamped = std::clamp(seconds, 0.0,
        std::max(0.0, duration - 1.0 / slot.sample_rate));
    if (slot.natural_fade && !slot.pause_pending && !slot.pending_stop) {
        slot.timeline.begin_from(now_, slot.paused ? 0.0 : 0.06,
            slot.timeline.gain_at(now_), slot.paused ? 0.0f : 1.0f);
        slot.natural_fade = false;
    }
    if (!submit_from(slot, clamped, failure)) {
        return false;
    }
    return true;
}

bool AudioEngine::set_bus_volume(AudioBus bus, float linear_gain, Failure& failure) {
    if (!initialized_) {
        failure.set(fail_code::backend_unavailable, "音频引擎尚未初始化");
        return false;
    }
    const float gain = std::clamp(linear_gain, 0.0f, 1.0f);
    IXAudio2SubmixVoice* voice = bus == AudioBus::bgm ? bgm_bus_voice_ : sfx_bus_voice_;
    const HRESULT hr = voice->SetVolume(gain);
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "设置总线音量失败 " + hresult_hex(hr));
        return false;
    }
    if (bus == AudioBus::bgm) {
        bgm_bus_volume_ = gain;
    } else {
        sfx_bus_volume_ = gain;
    }
    return true;
}

float AudioEngine::bus_volume(AudioBus bus) const noexcept {
    return bus == AudioBus::bgm ? bgm_bus_volume_ : sfx_bus_volume_;
}

bool AudioEngine::set_slot_volume(AudioBus bus, std::size_t slot_index, float linear_gain,
                                  Failure& failure) {
    if (!valid_slot(bus, slot_index, failure)) {
        return false;
    }
    Slot& slot = slot_at(bus, slot_index);
    slot.slot_volume = std::clamp(linear_gain, 0.0f, 1.0f);
    // 立刻生效，不等下一个 update —— 拖动音量条要跟手
    if (slot.voice && slot.in_use) {
        const float applied = slot.timeline.gain_at(slot.last_update_time) * slot.slot_volume;
        slot.voice->SetVolume(applied);
    }
    return true;
}

std::size_t AudioEngine::find_free_slot(AudioBus bus) const noexcept {
    const std::size_t count = slot_count(bus);
    for (std::size_t i = 0; i < count; ++i) {
        if (!slot_at(bus, i).in_use) {
            return i;
        }
    }
    return count;   // 满了
}

void AudioEngine::hard_stop(Slot& slot) noexcept {
    // DestroyVoice waits until the mixer no longer references the sample data.
    // A frame delay after FlushSourceBuffers is not an ownership guarantee.
    if (slot.stream) slot.stream->cancel();
    if (slot.voice) { slot.voice->DestroyVoice(); slot.voice = nullptr; }
    slot.stream.reset();
    slot.prepared.reset();
    slot.timeline.reset();
    slot.in_use = false;
    slot.paused = false;
    slot.pause_pending = false; slot.delayed_start = false; slot.natural_fade = false;
    slot.end_fade_seconds = 0;
    slot.pending_start = false;
    slot.pending_stop = false;
    slot.name.clear();
    slot.seek_offset_seconds = 0.0;
    slot.samples_played_at_seek = 0;
    std::vector<std::int16_t>().swap(slot.pcm);
}

void AudioEngine::update(double now_seconds) {
    if (!initialized_) {
        return;
    }

    now_ = now_seconds;
    const auto advance = [this, now_seconds](auto& collection) {
        for (Slot& slot : collection) {
            if (!slot.in_use || !slot.voice) {
                continue;
            }

            if (slot.stream) {
                Failure failure;
                if (slot.stream->take_failure(failure)) {
                    failures_.push_back(std::move(failure)); hard_stop(slot); continue;
                }
            }
            if (slot.delayed_start) {
                if (now_seconds < slot.start_after) continue;
                const HRESULT hr = slot.voice->Start(0);
                if (FAILED(hr)) {
                    Failure failure; failure.set(fail_code::platform_api_failed, "Delayed BGM Start " + hresult_hex(hr));
                    failures_.push_back(std::move(failure)); hard_stop(slot); continue;
                }
                slot.delayed_start = false;
            }
            // 落实 play() 挂起的起点请求
            if (slot.pending_start) {
                slot.timeline.begin(now_seconds, slot.pending_fade_in, 1.0f);
                slot.pending_start = false;
            }

            slot.last_update_time = now_seconds;

            // 暂停期间不推进任何东西：增益冻结在当前值
            if (slot.paused) {
                slot.voice->SetVolume(slot.timeline.gain_at(now_seconds) * slot.slot_volume);
                continue;
            }

            if (!slot.looping && slot.end_fade_seconds > 0 && !slot.timeline.stopping) {
                XAUDIO2_VOICE_STATE voice{}; slot.voice->GetState(&voice, 0);
                const double played = slot.seek_offset_seconds + static_cast<double>(voice.SamplesPlayed -
                    std::min(voice.SamplesPlayed, slot.samples_played_at_seek)) / slot.sample_rate;
                const double remaining = slot.duration_seconds() - played;
                const double tail = std::min(slot.end_fade_seconds, slot.duration_seconds() * 0.5);
                if (remaining <= tail) { slot.timeline.begin_stop(now_seconds, std::max(0.0, remaining)); slot.natural_fade = true; }
            }
            const float gain = slot.timeline.gain_at(now_seconds) * slot.slot_volume;
            const HRESULT volume_hr = slot.voice->SetVolume(gain);
            if (FAILED(volume_hr)) {
                Failure failure; failure.set(fail_code::platform_api_failed, "Fade SetVolume " + hresult_hex(volume_hr));
                failures_.push_back(std::move(failure)); hard_stop(slot); continue;
            }
            if (slot.pause_pending && slot.timeline.is_finished(now_seconds)) {
                const HRESULT hr = slot.voice->Stop(0);
                if (FAILED(hr)) {
                    Failure failure; failure.set(fail_code::platform_api_failed, "Fade pause Stop " + hresult_hex(hr));
                    failures_.push_back(std::move(failure)); hard_stop(slot); continue;
                }
                slot.paused = true; slot.pause_pending = false;
                continue;
            }

            // 淡出跑完，或（非循环时）缓冲被消费完 —— 回收槽位
            XAUDIO2_VOICE_STATE voice_state{};
            slot.voice->GetState(&voice_state, 0);

            const bool buffer_drained = !slot.looping && voice_state.BuffersQueued == 0 &&
                (!slot.stream || slot.stream->finished());
            if (slot.timeline.is_finished(now_seconds) || buffer_drained) {
                hard_stop(slot);
            }
        }
    };
    advance(bgm_slots_);
    advance(sfx_slots_);
}

VoiceSlotState AudioEngine::slot_state(AudioBus bus, std::size_t slot_index) const noexcept {
    VoiceSlotState state;
    if (slot_index >= slot_count(bus)) {
        return state;
    }
    const Slot& slot = slot_at(bus, slot_index);
    state.in_use = slot.in_use;
    state.paused = slot.paused || slot.pause_pending;
    state.stopping = slot.pending_stop || slot.natural_fade;
    state.looping = slot.looping;
    state.name = slot.name;
    state.duration_seconds = slot.duration_seconds();
    state.current_gain = slot.timeline.gain_at(slot.last_update_time) * slot.slot_volume;
    state.playing = slot.in_use && !slot.paused && state.current_gain > 0.0f;

    if (slot.in_use && slot.voice) {
        XAUDIO2_VOICE_STATE voice_state{};
        // 第二个参数传 0 = 连 SamplesPlayed 一起取（那个常量不存在，
        // XAUDIO2_VOICE_NOSAMPLESPLAYED 才是"跳过"的那个）
        slot.voice->GetState(&voice_state, 0);
        const std::uint64_t delta =
            voice_state.SamplesPlayed > slot.samples_played_at_seek
                ? voice_state.SamplesPlayed - slot.samples_played_at_seek
                : 0;
        double played = static_cast<double>(delta) / static_cast<double>(slot.sample_rate);
        played += slot.seek_offset_seconds;
        if (slot.looping && state.duration_seconds > 0.0) {
            played = std::fmod(played, state.duration_seconds);
        }
        state.position_seconds = std::max(0.0, played);
    }
    return state;
}

std::size_t AudioEngine::active_count(AudioBus bus) const noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < slot_count(bus); ++i) {
        if (slot_at(bus, i).in_use) {
            ++count;
        }
    }
    return count;
}

}  // namespace musicboard
