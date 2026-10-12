#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "audio/envelope_worker.h"
#include "audio/waveform.h"
#include "core/path_utf8.h"
#include "tests/test_files.h"
#include "tests/test_harness.h"

using namespace musicboard;
using namespace musicboard::test;

int wmain() {
    Registry r;
    TempDir temp;
    const auto wav = temp.path() / L"中文 曲目.wav";
    if (!write_test_wav(wav, 1000)) { r.check(false, "prepare WAV fixture"); return r.finish("envelope_worker_tests"); }
    EnvelopeWorker worker;
    // Simultaneous requests must all finish WITHOUT the caller re-enqueuing.
    for (std::size_t i = 0; i < 5; ++i) worker.request_audio(i, wav);
    bool got[5]{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && !std::all_of(std::begin(got), std::end(got), [](bool value) { return value; })) {
        for (std::size_t i = 0; i < 5; ++i) {
            if (got[i]) continue;
            DecodedAudio audio;
            bool success = false;
            Failure failure;
            if (worker.take_audio(i, audio, success, failure)) {
                got[i] = true;
                r.check_eq(success && audio.frame_count() == 8000,
                    "request delivered " + std::to_string(i), failure.to_string() + " frames=" + std::to_string(audio.frame_count()));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    r.check(std::all_of(std::begin(got), std::end(got), [](bool value) { return value; }), "all five independent playback requests finish");

    // Cancel/replace the same key with a missing file. Old success must not leak
    // through, and the original missing path/code must reach the consumer.
    worker.request_audio(20, wav);
    worker.cancel_audio(20);
    const auto missing = temp.path() / L"已删除的曲目.mp3";
    worker.request_audio(20, missing);
    DecodedAudio audio;
    bool success = true;
    Failure failure;
    bool delivered = false;
    const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!delivered && std::chrono::steady_clock::now() < failure_deadline) {
        delivered = worker.take_audio(20, audio, success, failure);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    r.check_eq(delivered && !success && failure.code == fail_code::file_not_found &&
        failure.context.find(path_to_utf8(missing)) != std::string::npos,
        "cancelled generation cannot overwrite the replacement failure", failure.to_string());

    failure.clear();
    std::vector<float> streamed;
    r.check_invocation([&] { return extract_waveform_file(wav, streamed, failure); }, "streaming waveform on Chinese WAV path", failure);
    r.check_invocation([&] { return decode_audio_file(wav, audio, failure); }, "decode WAV for independent peak comparison", failure);
    const auto expected = compute_waveform_envelope(audio, kDefaultWaveformBuckets, failure);
    r.check(streamed == expected, "streaming peaks match complete PCM without retaining it");

    // A pre-cancelled decode must return a concrete cancellation, no partial PCM.
    std::atomic<bool> cancelled{true};
    failure.clear();
    const bool decoded = decode_audio_file(wav, audio, failure, &cancelled);
    r.check_eq(!decoded && audio.samples.empty() && failure.code == "audio.cancelled",
               "chunk decoder handles cancellation explicitly", failure.to_string());

    wchar_t library_path[32768]{};
    const DWORD length = ::GetEnvironmentVariableW(L"MUSICBOARD_TEST_LIBRARY", library_path, 32768);
    if (length == 0 || length >= 32768) r.skip("real mp3/ogg waveform material not configured");
    else {
        bool mp3 = false, ogg = false;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(std::filesystem::path(library_path), ec), end;
             it != end && !ec && !(mp3 && ogg); it.increment(ec)) {
            const auto extension = it->path().extension();
            bool* seen = extension == L".mp3" ? &mp3 : extension == L".ogg" ? &ogg : nullptr;
            if (!seen || *seen || !it->is_regular_file(ec)) continue;
            *seen = true;
            failure.clear();
            const auto start = std::chrono::steady_clock::now();
            r.check_invocation([&] { return extract_waveform_file(it->path(), streamed, failure); },
                "real streaming waveform " + path_to_utf8(it->path().filename()), failure);
            r.check_eq(streamed.size() == kDefaultWaveformBuckets, "real waveform bucket count", std::to_string(streamed.size()));
            std::printf("waveform %s %.3fs\n", path_to_utf8(extension).c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
        if (!mp3) r.skip("no real MP3 material found");
        if (!ogg) r.skip("no real OGG material found");
    }
    return r.finish("envelope_worker_tests");
}
