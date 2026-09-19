#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include "audio/audio_engine.h"
#include "audio/preload_cache.h"
#include "library/library_loader.h"
#include "core/path_utf8.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"
using namespace musicboard;
using namespace musicboard::test;

namespace {
struct Collect final : AudioChunkConsumer {
    std::vector<std::int16_t> values;
    std::size_t limit_frames = 0;
    std::size_t received_frames = 0;
    [[nodiscard]] bool finished() const noexcept override { return limit_frames && received_frames >= limit_frames; }
    bool consume(const AudioFormat& format, const std::int16_t* samples, std::size_t frames, Failure&) override {
        const auto count = limit_frames ? std::min(frames, limit_frames - received_frames) : frames;
        values.insert(values.end(), samples, samples + count * format.channels);
        received_frames += count; return true;
    }
};
void pump(AudioEngine& engine, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    do {
        engine.update(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        ::Sleep(4);
    } while (std::chrono::steady_clock::now() < deadline);
}
void real_compressed_streams(Registry& r) {
    wchar_t root[32768]{};
    const auto length = ::GetEnvironmentVariableW(L"MUSICBOARD_TEST_LIBRARY", root, static_cast<DWORD>(std::size(root)));
    if (!length || length >= std::size(root)) { r.skip("real streaming library not configured"); return; }
    ScanOptions options; options.root = root;
    LibrarySnapshot snapshot;
    Failure failure;
    if (!scan_library(options, snapshot, failure)) { r.skip("real streaming library unavailable: " + failure.to_string()); return; }
    for (const auto kind : {AudioContainer::mp3, AudioContainer::ogg}) {
        const LibraryTrack* chosen = nullptr;
        for (const auto* section : {&snapshot.bgm, &snapshot.sfx}) for (const auto& category : *section)
            for (const auto& track : category.tracks) if (!chosen && track.container == kind && track.duration_seconds > 35.0) chosen = &track;
        const std::string label = std::string(container_name(kind));
        if (!chosen) { r.skip(label + " long real fixture absent"); continue; }
        Collect reference, suffix;
        reference.limit_frames = chosen->format.sample_rate * 5;
        suffix.limit_frames = chosen->format.sample_rate * 2;
        failure.clear();
        const bool read_reference = read_audio_chunks(chosen->path, reference, failure);
        r.check_eq(read_reference, label + " independent sequential reference", failure.to_string());
        failure.clear();
        const bool read_suffix = read_audio_chunks(chosen->path, suffix, failure, nullptr, 2.0);
        r.check_eq(read_suffix, label + " seek to preloaded boundary", failure.to_string());
        const auto offset = chosen->format.sample_rate * chosen->format.channels * 2;
        const bool matches = read_reference && read_suffix && !suffix.values.empty() &&
            reference.values.size() >= offset + suffix.values.size() &&
            std::equal(suffix.values.begin(), suffix.values.end(), reference.values.begin() + offset);
        r.check_eq(matches, label + " streamed boundary matches sequential PCM exactly", path_to_utf8(chosen->path));
        std::shared_ptr<const PreparedAudio> prepared;
        AudioInfo info{chosen->container, chosen->format, chosen->duration_seconds};
        failure.clear();
        const bool ready = prepare_audio(chosen->path, info, prepared, failure);
        r.check_eq(ready, label + " real prefix prepares", failure.to_string());
        AudioEngine engine;
        failure.clear();
        if (!engine.initialize(failure)) { r.skip(label + " output device unavailable"); continue; }
        if (ready) {
            r.check_invocation([&] { return engine.play_prepared(AudioBus::bgm, 0, prepared, label, 0, false, failure); }, label + " real streamed voice starts", failure);
            ::Sleep(2400); pump(engine, 0.02);
            const auto state = engine.slot_state(AudioBus::bgm, 0);
            r.check_eq(state.playing && state.position_seconds > 2.25, label + " real voice crosses prefix during UI stall", std::to_string(state.position_seconds));
            Failure async;
            const bool failed = engine.take_failure(async);
            r.check_eq(!failed, label + " stream has no backend error", async.to_string());
        }
    }
}
}
int wmain() {
    Registry r;
    TempDir temp;
    const auto long_path = temp.path() / L"长曲 中文.wav";
    const auto short_path = temp.path() / L"短音效.wav";
    if (!write_test_wav(long_path, 35000) || !write_test_wav(short_path, 900)) {
        r.check(false, "write preload fixtures"); return r.finish("preload_stream_tests");
    }
    Failure failure;
    AudioInfo info;
    std::shared_ptr<const PreparedAudio> long_audio, short_audio;
    r.check_invocation([&] { return probe_audio_file(long_path, info, failure); }, "probe long WAV", failure);
    r.check_invocation([&] { return prepare_audio(long_path, info, long_audio, failure); }, "preload long WAV", failure);
    r.check_eq(long_audio && !long_audio->complete && long_audio->head.frame_count() == 16000 && long_audio->total_frames == 280000,
        "long tracks retain exactly two seconds, metadata retains complete length",
        long_audio ? std::to_string(long_audio->head.frame_count()) + "/" + std::to_string(long_audio->total_frames) : "no data");
    r.check_invocation([&] { return probe_audio_file(short_path, info, failure); }, "probe short WAV", failure);
    r.check_invocation([&] { return prepare_audio(short_path, info, short_audio, failure); }, "preload short WAV", failure);
    r.check(short_audio && short_audio->complete && short_audio->head.frame_count() == 7200, "short clip is complete in memory");
    r.check(long_audio && long_audio->envelope.empty(), "startup does not decode long tails just to draw waveforms");
    std::shared_ptr<const PreparedAudio> cached;
    bool cache_hit = false;
    std::string note;
    const auto cache_root = temp.path() / L"缓存";
    r.check_invocation([&] { return prepare_audio_cached(short_path, info, cache_root, cached, cache_hit, note, failure, nullptr, {}); }, "publish short audio cache", failure);
    r.check(!cache_hit && note.empty(), "first cache access actually prepares audio");
    r.check_invocation([&] { return prepare_audio_cached(short_path, info, cache_root, cached, cache_hit, note, failure, nullptr, {}); }, "load prepared cache", failure);
    r.check(cache_hit && cached->head.samples == short_audio->head.samples, "warm load reuses exact prepared samples");
    std::error_code cache_error;
    const auto cache_it = std::filesystem::directory_iterator(cache_root, cache_error);
    if (cache_error || cache_it == std::filesystem::directory_iterator{}) {
        r.check_eq(false, "cache fixture exists", cache_error.message());
        return r.finish("preload_stream_tests");
    }
    const auto cache_file = cache_it->path();
    r.check(write_text(cache_file, "damaged cache"), "corrupt cache fixture");
    r.check_invocation([&] { return prepare_audio_cached(short_path, info, cache_root, cached, cache_hit, note, failure, nullptr, {}); }, "repair invalid cache", failure);
    r.check(!cache_hit && !note.empty() && cached->head.samples == short_audio->head.samples,
        "corrupt cache is explained and regenerated, never played");
    DecodedAudio full;
    Collect tail;
    r.check_invocation([&] { return decode_audio_file(long_path, full, failure); }, "decode independent full WAV reference", failure);
    r.check_invocation([&] { return read_audio_chunks(long_path, tail, failure, nullptr, 2.0); }, "stream continuation from two seconds", failure);
    r.check_eq(tail.values.size() == full.samples.size() - 16000 &&
        std::equal(tail.values.begin(), tail.values.end(), full.samples.begin() + 16000),
        "preloaded head and streamed suffix match full PCM without overlap or gaps", std::to_string(tail.values.size()));

    std::atomic<bool> cancelled{true};
    std::shared_ptr<const PreparedAudio> aborted;
    failure.clear();
    const bool aborted_ok = prepare_audio(short_path, info, aborted, failure, &cancelled);
    r.check_eq(!aborted_ok && !aborted && failure.code == "audio.cancelled", "cancelled preload never publishes partial data", failure.to_string());
    LibraryLoader loader;
    failure.clear();
    r.check_invocation([&] { return loader.start(temp.path(), failure); }, "start actual loading transaction", failure);
    LibraryLoadResult result;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool got = false;
    while (!(got = loader.take(result)) && std::chrono::steady_clock::now() < deadline) ::Sleep(2);
    r.check_eq(got && result.failure.empty() && result.tracks.size() == 2 && loader.progress().fraction == 1.0,
        "loader publishes two ready clips and truthful completion", result.failure.to_string());
    r.check_invocation([&] { return loader.start(temp.path(), failure); }, "restart library loading", failure);
    loader.cancel();
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!(got = loader.take(result)) && std::chrono::steady_clock::now() < deadline) ::Sleep(2);
    r.check(got && result.cancelled, "reload cancellation is explicitly reported");
    const auto absent = temp.path() / L"不存在";
    r.check_invocation([&] { return loader.start(absent, failure); }, "queue missing library path", failure);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!(got = loader.take(result)) && std::chrono::steady_clock::now() < deadline) ::Sleep(2);
    r.check_eq(got && !result.failure.empty() && result.failure.context.find(path_to_utf8(absent)) != std::string::npos,
        "missing library cannot become successful empty library", result.failure.to_string());

    AudioEngine engine;
    failure.clear();
    if (!engine.initialize(failure)) r.skip("audio device unavailable: " + failure.to_string());
    else {
        r.check_invocation([&] { return engine.configure_sfx_slots(12, failure); }, "independent DJ voice slots", failure);
        double worst_ms = 0.0;
        for (std::size_t i = 4; i < 12; ++i) {
            const auto start = std::chrono::steady_clock::now();
            r.check_invocation([&] { return engine.play_prepared(AudioBus::sfx, i, short_audio, "pad", 0.0, false, failure); }, "start independent pad " + std::to_string(i), failure);
            worst_ms = std::max(worst_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        pump(engine, 0.05);
        r.check_eq(engine.active_count(AudioBus::sfx) == 8, "eight pads play together without replacing other buttons", std::to_string(engine.active_count(AudioBus::sfx)));
        r.check_eq(worst_ms < 80.0, "preloaded click submits immediately", "worst_ms=" + std::to_string(worst_ms));
        std::printf("PAD_SUBMIT_WORST_MS=%.3f\n", worst_ms);
        pump(engine, 1.0);
        r.check(engine.active_count(AudioBus::sfx) == 0, "all DJ clips finish once without looping");
        r.check_invocation([&] { return engine.play_prepared(AudioBus::bgm, 0, long_audio, "stream", 0.0, true, failure); }, "start long preloaded head", failure);
        // The UI does not pump for longer than the head: the producer must keep supplying audio.
        ::Sleep(4400);
        pump(engine, 0.04);
        const auto advanced = engine.slot_state(AudioBus::bgm, 0);
        r.check_eq(advanced.in_use && advanced.position_seconds > 4.2 && advanced.position_seconds < 5.0,
            "audio progresses past cached head even when UI stalls", std::to_string(advanced.position_seconds));
        r.check_invocation([&] { return engine.seek(AudioBus::bgm, 0, 34.85, failure); }, "seek streamed voice near end", failure);
        pump(engine, 0.45);
        const auto looped = engine.slot_state(AudioBus::bgm, 0);
        r.check_eq(looped.in_use && looped.position_seconds < 1.0, "streamed loop continues at full-song beginning", std::to_string(looped.position_seconds));
        r.check_invocation([&] { return engine.pause(AudioBus::bgm, 0, failure); }, "pause streamed voice", failure);
        r.check_invocation([&] { return engine.stop(AudioBus::bgm, 0, 1.0, failure); }, "clear paused stream cancels its producer", failure);
        r.check(!engine.slot_state(AudioBus::bgm, 0).in_use, "cleared stream cannot revive");
        Failure asynchronous;
        const bool stream_failed = engine.take_failure(asynchronous);
        r.check_eq(!stream_failed, "streaming has no asynchronous backend failures", asynchronous.to_string());
    }
    real_compressed_streams(r);
    return r.finish("preload_stream_tests");
}
