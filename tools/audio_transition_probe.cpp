#include "audio/audio_engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace musicboard;
namespace {
DecodedAudio tone(double hz, double seconds) {
    DecodedAudio audio;
    audio.format = AudioFormat{48000, 2, 16};
    audio.duration_seconds = seconds;
    const auto frames = static_cast<std::size_t>(seconds * 48000);
    audio.samples.resize(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto value = static_cast<std::int16_t>(6000 * std::sin(6.283185307179586 * hz * static_cast<double>(frame) / 48000));
        audio.samples[2*frame] = value; audio.samples[2*frame+1] = value;
    }
    return audio;
}
[[nodiscard]] bool run(AudioEngine& engine, Failure& failure) {
    const auto origin = std::chrono::steady_clock::now();
    const auto now = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now()-origin).count(); };
    const auto wait = [&](double seconds) {
        const double until = now() + seconds;
        while (now() < until) { engine.update(now()); std::this_thread::sleep_for(std::chrono::milliseconds(8)); }
    };
    const auto mark = [&](const char* phase) { std::printf("PHASE %s %.6f\n", phase, now()); std::fflush(stdout); };
    const auto a = tone(330, 8), b = tone(660, 8), short_tone = tone(990, 1.6);
    engine.update(now());
    mark("start_fade");
    if (!engine.play(AudioBus::bgm, 0, a, "tone A", 0.8, true, failure)) return false;
    wait(1.3);
    mark("pause");
    if (!engine.pause(AudioBus::bgm, 0, failure, 0.6)) return false;
    wait(1.0);
    const double paused_position = engine.slot_state(AudioBus::bgm, 0).position_seconds;
    wait(0.2);
    if (std::abs(engine.slot_state(AudioBus::bgm, 0).position_seconds-paused_position) > 0.02) {
        failure.set(fail_code::invalid_argument, "Paused source position still advances"); return false;
    }
    mark("resume");
    if (!engine.resume(AudioBus::bgm, 0, failure, 0.8)) return false;
    wait(1.2);
    mark("crossfade");
    if (!engine.play(AudioBus::bgm, 1, b, "tone B", 2.5, true, failure) ||
        !engine.transition_bgm(0, 1, 2.5, true, failure)) return false;
    wait(3.0);
    if (engine.active_count(AudioBus::bgm) != 1) {
        failure.set(fail_code::invalid_argument, "Outgoing voice was not reclaimed after crossfade"); return false;
    }
    mark("stop");
    if (!engine.stop(AudioBus::bgm, 1, 0.6, failure)) return false;
    wait(1.0);
    mark("natural_tail");
    if (!engine.play(AudioBus::sfx, 0, short_tone, "short effect", 0.35, false, failure)) return false;
    wait(2.0);
    mark("sequential_start");
    if (!engine.play(AudioBus::bgm, 0, a, "tone A", 0.0, true, failure)) return false;
    wait(0.4);
    mark("sequential_switch");
    if (!engine.play(AudioBus::bgm, 1, b, "tone B", 0.4, true, failure) ||
        !engine.transition_bgm(0, 1, 0.4, false, failure)) return false;
    wait(1.2);
    mark("done");
    if (!engine.stop(AudioBus::bgm, 1, 0, failure)) return false;
    wait(0.15);
    if (engine.take_failure(failure)) return false;
    return true;
}
}
int wmain() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AudioEngine engine; Failure failure;
    if (!engine.initialize(failure)) { std::printf("SKIP %s\n", failure.to_string().c_str()); return 77; }
    if (!run(engine, failure)) { std::printf("FAIL %s\n", failure.to_string().c_str()); return 1; }
    std::printf("PASS transition probe\n"); return 0;
}
