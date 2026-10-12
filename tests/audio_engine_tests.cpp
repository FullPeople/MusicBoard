// 音频引擎测试（真 XAudio2）
//
// 这一层要验的是"引擎真的能用"，不是"声音好不好听"（那个只能靠人耳）。
// 所以用例集中在可断言的状态与数值上：声部活着没有、增益是多少、位置走没走。
//
// 最要紧的两条：
//   1. **时间跳跃后增益仍然是正确值** —— 这是修掉"每帧 delta 累加"那个历史问题的判据
//   2. **暂停真的冻结了播放位置** —— 不是把音量调 0 假装暂停
//
// 没有音频输出设备时（虚拟机、远程桌面）整体 SKIP 而不是 FAIL ——
// 测试的成败不该取决于跑在哪台机器上。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/source_buffer.h"
#include "audio/decoder.h"
#include "core/path_utf8.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

double monotonic_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 合成一段 16 位 PCM：44100Hz / 2 声道 / 指定时长。
// 用静音就行 —— 这一层验的是引擎行为，不是音频内容。
DecodedAudio make_pcm(double seconds, std::uint32_t rate = 44100, std::uint16_t channels = 2) {
    DecodedAudio audio;
    audio.format.sample_rate = rate;
    audio.format.channels = channels;
    audio.format.bits_per_sample = 16;
    audio.duration_seconds = seconds;
    audio.backend = "synthetic";
    const std::size_t frames = static_cast<std::size_t>(seconds * rate);
    audio.samples.assign(frames * channels, 0);
    return audio;
}

// 虚拟时钟：锚定在真实时间上，但由测试自己推进。
//
// 为什么不能用"每次调用都重新取 monotonic_now()" —— 那样第二次推进不是从第一次结束的地方
// 接续，累计的虚拟时间会比设想的小（实测因此误报"淡入没到满增益"）。
// 所有涉及淡入淡出的用例都必须用同一个连续的虚拟时钟。
struct VirtualClock final {
    double base = monotonic_now();
    double elapsed = 0.0;

    [[nodiscard]] double now() const { return base + elapsed; }

    void advance(AudioEngine& engine, double seconds, int steps = 4) {
        for (int i = 1; i <= steps; ++i) {
            engine.update(base + elapsed + seconds * static_cast<double>(i) /
                                             static_cast<double>(steps));
        }
        elapsed += seconds;
    }
};

// 文件级虚拟时钟：用例共用一条连续时间线，每个用例开头 reset。
VirtualClock g_clock;

void reset_clock() {
    g_clock = VirtualClock{};
}

void pump(AudioEngine& engine, double seconds, int steps = 4) {
    g_clock.advance(engine, seconds, steps);
}

// 需要真实等待的用例（暂停位置、缓冲消费）用它 —— 位置来自 XAudio2 的真实时间，
// 而虚拟时钟也要跟着走，否则淡入淡出的推算会落后。
void pump_with_real_wait(AudioEngine& engine, double real_seconds) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<int>(real_seconds * 1000.0)));
    g_clock.advance(engine, real_seconds, 2);
}

// 清理用的停止：这些位置不关心结果，但 [[nodiscard]] 要求把「有意忽略」写出来
void stop_and_ignore(AudioEngine& engine, AudioBus bus, std::size_t slot) {
    Failure ignored;
    static_cast<void>(engine.stop(bus, slot, 0.0, ignored));
    pump(engine, 0.05);
}

struct Fixture final {
    AudioEngine engine;
    bool ready = false;
    Failure failure;

    [[nodiscard]] bool initialize() {
        if (!engine.initialize(failure)) {
            return false;
        }
        ready = true;
        return true;
    }
};

void test_initialize_reports_output_format(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("没有可用的音频输出设备，跳过全部引擎用例");
        return;
    }
    r.check(fixture.engine.initialized(), "引擎初始化成功");
    r.check(fixture.engine.output_sample_rate() > 0, "主声部采样率有效");
    r.check(fixture.engine.output_channels() > 0, "主声部声道数有效");
    r.check(std::string(fixture.engine.backend_name()) == "xaudio2", "后端名是 xaudio2");
    // 这一条信息值得打出来：本机主声部是 48000，而曲库是 44100，
    // 说明重采样确实由 XAudio2 负责，上层不用管。
    std::printf("      主声部: %u Hz / %u 声道\n", fixture.engine.output_sample_rate(),
                fixture.engine.output_channels());
}

void test_play_and_state(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    const DecodedAudio audio = make_pcm(2.0);

    Failure failure;
    r.check_invocation([&] { return engine.play(AudioBus::bgm, 0, audio, "测试曲目", 0.0, false, failure); }, "播放成功", failure);
    pump(engine, 0.05);

    const VoiceSlotState state = engine.slot_state(AudioBus::bgm, 0);
    r.check(state.in_use, "槽位被占用");
    r.check(state.playing, "声部在播");
    r.check(!state.paused, "未暂停");
    r.check(std::fabs(state.duration_seconds - 2.0) < 0.01, "时长约 2 秒");
    r.check(state.name == "测试曲目", "曲名记在槽位上");
    r.check(engine.active_count(AudioBus::bgm) == 1, "BGM 活跃声部数为 1");
    r.check(engine.active_count(AudioBus::sfx) == 0, "SFX 总线不受影响");

    r.check(engine.stop(AudioBus::bgm, 0, 0.0, failure), "立即停止成功");
    pump(engine, 0.05);
    r.check(!engine.slot_state(AudioBus::bgm, 0).in_use, "停止后槽位释放");
}

void test_fade_in_reaches_full_gain(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    const DecodedAudio audio = make_pcm(5.0);

    Failure failure;
    r.check(engine.play(AudioBus::bgm, 0, audio, "淡入测试", 2.0, false, failure), "带淡入播放");

    // 起点：增益应当接近 0
    engine.update(g_clock.now());
    const float at_start = engine.slot_state(AudioBus::bgm, 0).current_gain;

    // 推到淡入中途
    pump(engine, 1.0, 2);
    const float midway = engine.slot_state(AudioBus::bgm, 0).current_gain;

    // 推到淡入之后
    pump(engine, 1.5, 2);
    const float after = engine.slot_state(AudioBus::bgm, 0).current_gain;

    r.check_eq(at_start < 0.2f, "淡入起点增益接近 0",
               "起点增益=" + std::to_string(at_start));
    r.check_eq(midway > at_start && midway < 0.95f, "中途增益在 0 与满值之间",
               "中途增益=" + std::to_string(midway));
    r.check_eq(after > 0.98f, "淡入结束后到达满增益",
               "结束后增益=" + std::to_string(after));

    stop_and_ignore(engine, AudioBus::bgm, 0);
}

// ---------------------------------------------------------------------------
// 这一条是修掉"每帧 delta 累加"那个问题的判据
// ---------------------------------------------------------------------------
void test_gain_survives_a_long_stall(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    const DecodedAudio audio = make_pcm(30.0);

    Failure failure;
    r.check(engine.play(AudioBus::bgm, 0, audio, "卡顿测试", 1.0, false, failure), "带 1 秒淡入播放");

    const double t0 = monotonic_now();
    engine.update(t0);                 // 落实起点
    engine.update(t0 + 0.2);           // 淡入到 20%
    const float early = engine.slot_state(AudioBus::bgm, 0).current_gain;

    // 模拟卡顿：整整 20 秒不调用 update（远超过 1 秒的淡入）
    engine.update(t0 + 20.0);
    const float after_stall = engine.slot_state(AudioBus::bgm, 0).current_gain;

    r.check_eq(early < 0.4f, "卡顿前增益还很低", "增益=" + std::to_string(early));
    r.check_eq(after_stall > 0.98f,
               "卡顿 20 秒后第一次 update 就把增益落到正确值（不是停在半途）",
               "增益=" + std::to_string(after_stall));

    stop_and_ignore(engine, AudioBus::bgm, 0);
}

void test_pause_freezes_position(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    const DecodedAudio audio = make_pcm(10.0);

    Failure failure;
    r.check(engine.play(AudioBus::bgm, 0, audio, "暂停测试", 0.0, false, failure), "开始播放");

    // 让它真的播一会儿
    pump(engine, 0.3);
    pump_with_real_wait(engine, 0.2);
    const double before_pause = engine.slot_state(AudioBus::bgm, 0).position_seconds;

    r.check(engine.pause(AudioBus::bgm, 0, failure), "暂停成功");
    r.check(engine.slot_state(AudioBus::bgm, 0).paused, "状态标记为已暂停");

    // 暂停期间等一会儿，位置不该动
    pump_with_real_wait(engine, 0.3);
    const double during_pause = engine.slot_state(AudioBus::bgm, 0).position_seconds;

    r.check_eq(std::fabs(during_pause - before_pause) < 0.08,
               "暂停期间播放位置冻结（不是把音量调 0 假装暂停）",
               "暂停前=" + std::to_string(before_pause) + " 暂停期间=" + std::to_string(during_pause));
    r.check(before_pause > 0.02, "暂停前确实播了一点（否则这条测不出东西）");

    r.check(engine.resume(AudioBus::bgm, 0, failure), "恢复成功");
    r.check(!engine.slot_state(AudioBus::bgm, 0).paused, "状态标记为未暂停");

    pump_with_real_wait(engine, 0.25);
    const double after_resume = engine.slot_state(AudioBus::bgm, 0).position_seconds;
    r.check_eq(after_resume > during_pause + 0.1, "恢复后位置继续推进",
               "恢复前=" + std::to_string(during_pause) + " 恢复后=" + std::to_string(after_resume));

    stop_and_ignore(engine, AudioBus::bgm, 0);
}

void test_fade_out_then_release(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    const DecodedAudio audio = make_pcm(10.0);

    Failure failure;
    r.check(engine.play(AudioBus::sfx, 0, audio, "淡出测试", 0.0, false, failure), "开始播放");
    pump(engine, 0.05);
    r.check(engine.slot_state(AudioBus::sfx, 0).current_gain > 0.98f, "满增益");

    r.check(engine.stop(AudioBus::sfx, 0, 1.0, failure), "请求 1 秒淡出");
    engine.update(g_clock.now());
    const float at_stop = engine.slot_state(AudioBus::sfx, 0).current_gain;

    pump(engine, 0.5, 2);
    const float midway = engine.slot_state(AudioBus::sfx, 0).current_gain;

    pump(engine, 0.7, 2);
    const bool released = !engine.slot_state(AudioBus::sfx, 0).in_use;

    r.check_eq(at_stop > 0.9f, "淡出起点仍是满增益", "增益=" + std::to_string(at_stop));
    r.check_eq(midway < at_stop && midway > 0.0f, "淡出中途在 0 与起点之间",
               "增益=" + std::to_string(midway));
    r.check(released, "淡出跑完后槽位被回收");
}

void test_buses_are_independent(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    Failure failure;

    r.check(engine.set_bus_volume(AudioBus::bgm, 0.3f, failure), "设置 BGM 总线音量");
    r.check(engine.set_bus_volume(AudioBus::sfx, 0.8f, failure), "设置 SFX 总线音量");
    r.check(std::fabs(engine.bus_volume(AudioBus::bgm) - 0.3f) < 0.01f, "BGM 音量记为 0.3");
    r.check(std::fabs(engine.bus_volume(AudioBus::sfx) - 0.8f) < 0.01f, "SFX 音量记为 0.8");

    // 改一条不影响另一条
    r.check(engine.set_bus_volume(AudioBus::bgm, 0.9f, failure), "再改 BGM 音量");
    r.check(std::fabs(engine.bus_volume(AudioBus::sfx) - 0.8f) < 0.01f,
            "改 BGM 不影响 SFX 总线");

    // 越界要被夹住
    r.check(engine.set_bus_volume(AudioBus::bgm, 5.0f, failure), "超范围音量不报错");
    r.check(std::fabs(engine.bus_volume(AudioBus::bgm) - 1.0f) < 0.01f, "超范围音量被夹到 1");

    r.check(engine.set_bus_volume(AudioBus::bgm, 1.0f, failure), "恢复 BGM 音量");
}

void test_slots_are_independent(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    const DecodedAudio audio = make_pcm(10.0);
    Failure failure;

    r.check(engine.play(AudioBus::sfx, 0, audio, "台1", 0.0, false, failure), "台 1 播放");
    r.check(engine.play(AudioBus::sfx, 1, audio, "台2", 0.0, false, failure), "台 2 播放");
    pump(engine, 0.05);

    r.check(engine.active_count(AudioBus::sfx) == 2, "两台同时活跃");

    r.check(engine.set_slot_volume(AudioBus::sfx, 0, 0.2f, failure), "台 1 音量设为 0.2");
    pump(engine, 0.02);
    const float gain0 = engine.slot_state(AudioBus::sfx, 0).current_gain;
    const float gain1 = engine.slot_state(AudioBus::sfx, 1).current_gain;

    r.check_eq(std::fabs(gain0 - 0.2f) < 0.05f, "台 1 增益跟着自己的音量",
               "台1增益=" + std::to_string(gain0));
    r.check_eq(gain1 > 0.95f, "台 2 不受台 1 音量影响",
               "台2增益=" + std::to_string(gain1));

    stop_and_ignore(engine, AudioBus::sfx, 0);
    stop_and_ignore(engine, AudioBus::sfx, 1);
}

void test_looping_does_not_release(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    const DecodedAudio audio = make_pcm(0.3);   // 很短，好让它"播完"
    Failure failure;

    r.check(engine.play(AudioBus::bgm, 0, audio, "循环测试", 0.0, true, failure), "循环播放");
    pump(engine, 0.05);
    r.check(engine.slot_state(AudioBus::bgm, 0).looping, "槽位标记为循环");

    // 等超过一个完整时长，循环播放下不该被回收
    pump_with_real_wait(engine, 0.6);
    r.check_eq(engine.slot_state(AudioBus::bgm, 0).in_use,
               "循环播放下播完一遍不会被回收（XAUDIO2_LOOP_INFINITE）",
               "时长=0.3s，已等 0.6s");

    // 位置应当回到开头附近（fmod 到时长以内）
    const double position = engine.slot_state(AudioBus::bgm, 0).position_seconds;
    r.check_eq(position >= 0.0 && position < 0.35, "循环中报告的进度落在时长范围内",
               "位置=" + std::to_string(position));

    stop_and_ignore(engine, AudioBus::bgm, 0);
}

void test_non_looping_releases_after_drain(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    const DecodedAudio audio = make_pcm(0.2);
    Failure failure;

    r.check(engine.play(AudioBus::sfx, 0, audio, "短音效", 0.0, false, failure), "播放 0.2 秒的音效");
    pump(engine, 0.05);
    r.check(engine.slot_state(AudioBus::sfx, 0).in_use, "刚开始时槽位占用");

    pump_with_real_wait(engine, 0.5);
    r.check_eq(!engine.slot_state(AudioBus::sfx, 0).in_use,
               "非循环音效播完后自动回收槽位",
               "时长=0.2s，已等 0.5s");
}

void test_find_free_slot_and_exhaustion(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    const DecodedAudio audio = make_pcm(10.0);
    Failure failure;

    // 占满四台
    for (std::size_t i = 0; i < AudioEngine::sfx_slot_count; ++i) {
        r.check(engine.play(AudioBus::sfx, i, audio, "占位" + std::to_string(i), 0.0, false, failure),
                "占满音效台 " + std::to_string(i));
    }
    pump(engine, 0.05);

    r.check_eq(engine.find_free_slot(AudioBus::sfx) == AudioEngine::sfx_slot_count,
               "四台占满时 find_free_slot 报告没有空位",
               "返回值=" + std::to_string(engine.find_free_slot(AudioBus::sfx)));
    r.check(engine.find_free_slot(AudioBus::bgm) == 0, "BGM 总线仍有空位（两条独立）");

    // 越界下标要被明确拒绝
    Failure range_failure;
    r.check(!engine.play(AudioBus::sfx, 99, audio, "越界", 0.0, false, range_failure),
            "越界槽位下标被拒绝");
    r.check(range_failure.code == fail_code::invalid_argument, "越界给出 invalid_argument");

    for (std::size_t i = 0; i < AudioEngine::sfx_slot_count; ++i) {
        // 清理路径不关心结果，但要显式写出「有意忽略」
        Failure ignored;
        static_cast<void>(engine.stop(AudioBus::sfx, i, 0.0, ignored));
    }
    pump(engine, 0.05);
}

void test_seek_changes_position(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    AudioEngine& engine = fixture.engine;
    const DecodedAudio audio = make_pcm(20.0);
    Failure failure;

    r.check(engine.play(AudioBus::bgm, 0, audio, "跳转测试", 0.0, false, failure), "开始播放");
    pump(engine, 0.05);

    r.check(engine.seek(AudioBus::bgm, 0, 12.5, failure), "跳转到 12.5 秒");
    pump(engine, 0.05);
    const double position = engine.slot_state(AudioBus::bgm, 0).position_seconds;

    r.check_eq(std::fabs(position - 12.5) < 0.5, "跳转后位置约 12.5 秒",
               "位置=" + std::to_string(position));

    stop_and_ignore(engine, AudioBus::bgm, 0);
}

void test_uninitialized_engine_fails_cleanly(Registry& r) {
    reset_clock();
    // 没初始化的引擎不该崩，也不该假装成功
    AudioEngine engine;
    Failure failure;
    const DecodedAudio audio = make_pcm(1.0);
    r.check(!engine.play(AudioBus::bgm, 0, audio, "x", 0.0, false, failure),
            "未初始化的引擎拒绝播放");
    r.check(failure.code == fail_code::backend_unavailable, "给出 backend_unavailable");
    r.check(!engine.set_bus_volume(AudioBus::bgm, 0.5f, failure), "未初始化时设置音量也失败");
    engine.update(g_clock.now());   // 不该崩
    r.check(true, "未初始化时 update 是安全的空操作");
}

void test_real_file_end_to_end(Registry& r, Fixture& fixture) {
    reset_clock();
    if (!fixture.ready) {
        r.skip("无输出设备");
        return;
    }
    // 必须用宽字符 API 读环境变量：getenv 在 Windows 上按活动代码页返回，
    // 含中文的曲库路径会被打乱（decoder_tests 里已经踩过一次）。
    wchar_t buffer[32768]{};
    const DWORD written = ::GetEnvironmentVariableW(L"MUSICBOARD_TEST_LIBRARY", buffer,
                                                   static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        r.skip("未设置 MUSICBOARD_TEST_LIBRARY，跳过真实文件解码→播放的端到端用例");
        return;
    }
    const std::filesystem::path root(buffer);
    if (!std::filesystem::is_directory(root)) {
        r.skip("曲库目录不存在");
        return;
    }

    // 找一个 ogg（走 stb_vorbis 后端）和它所在的真实中文路径
    std::filesystem::path target;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator() && target.empty(); it.increment(ec)) {
        if (ec) {
            break;
        }
        if (it->is_regular_file(ec) && path_to_utf8(it->path().extension()) == ".ogg") {
            target = it->path();
        }
    }
    if (target.empty()) {
        r.skip("曲库里没找到 ogg");
        return;
    }

    // 完整链路：中文路径 → 解码（stb_vorbis）→ 交给引擎播
    DecodedAudio audio;
    Failure decode_failure;
    r.check_eq(decode_audio_file(target, audio, decode_failure), "解码真实 ogg（中文路径）",
               path_to_utf8(target) + " → " + decode_failure.to_string());
    if (audio.samples.empty()) {
        return;
    }

    Failure play_failure;
    r.check_invocation([&] { return fixture.engine.play(AudioBus::bgm, 0, audio, path_to_utf8(target.filename()), 0.2,
                                   false, play_failure); }, "把解出来的 PCM 交给引擎播放", play_failure);
    pump(fixture.engine, 0.4);

    const VoiceSlotState state = fixture.engine.slot_state(AudioBus::bgm, 0);
    r.check(state.in_use, "真实曲目在播");
    r.check_eq(std::fabs(state.duration_seconds - audio.duration_seconds) < 0.1,
               "引擎报告的时长与解码器一致",
               "引擎=" + std::to_string(state.duration_seconds) +
                   " 解码=" + std::to_string(audio.duration_seconds));
    r.check(state.current_gain > 0.5f, "淡入已推进");

    stop_and_ignore(fixture.engine, AudioBus::bgm, 0);
}

void test_lifecycle_regressions(Registry& r, Fixture& fixture) {
    if (!fixture.ready) { r.skip("audio lifecycle needs an output device"); return; }
    auto& engine = fixture.engine;
    reset_clock();
    Failure failure;
    const auto stereo = make_pcm(1.0, 44100, 2);
    const auto mono = make_pcm(0.5, 8000, 1);
    r.check_invocation([&] { return engine.play(AudioBus::bgm, 0, stereo, "stereo", 0, true, failure); }, "start stereo", failure);
    pump(engine, 0.02);
    r.check_invocation([&] { return engine.play(AudioBus::bgm, 0, mono, "mono", 0, false, failure); }, "replace stereo with 8kHz mono", failure);
    pump(engine, 0.02);
    const auto duration = engine.slot_state(AudioBus::bgm, 0).duration_seconds;
    r.check_eq(std::fabs(duration - 0.5) < 0.001, "replacement uses the new sample format", std::to_string(duration));
    pump_with_real_wait(engine, 0.2);
    const auto position = engine.slot_state(AudioBus::bgm, 0).position_seconds;
    r.check_eq(position > 0.1 && position < 0.4, "mono playback advances at the correct speed", std::to_string(position));
    r.check_invocation([&] { return engine.pause(AudioBus::bgm, 0, failure); }, "pause before clear", failure);
    r.check_invocation([&] { return engine.stop(AudioBus::bgm, 0, 0.2, failure); }, "clear a paused voice", failure);
    pump(engine, 0.4);
    r.check(!engine.slot_state(AudioBus::bgm, 0).in_use, "paused voice is released by clear");
    r.check_invocation([&] { return engine.play(AudioBus::bgm, 0, stereo, "cancel fade", 1.2, true, failure); }, "request fade-in", failure);
    r.check_invocation([&] { return engine.stop(AudioBus::bgm, 0, 0.2, failure); }, "stop before first update", failure);
    pump(engine, 2.0);
    r.check(!engine.slot_state(AudioBus::bgm, 0).in_use, "deferred fade-in does not resurrect a stopped voice");
    failure.clear();
    const bool invalid = engine.set_loop(AudioBus::sfx, AudioEngine::sfx_slot_count, true, failure);
    r.check_eq(!invalid && failure.code == fail_code::invalid_argument,
               "loop rejects an out-of-bounds deck", failure.to_string());

    failure.clear();
    XAUDIO2_BUFFER buffer{};
    std::vector<std::int16_t> sentinel(100);
    for (std::size_t i = 0; i < sentinel.size(); ++i) sentinel[i] = static_cast<std::int16_t>(i);
    r.check_invocation([&] { return make_source_buffer(sentinel, 1, 80, true, buffer, failure); }, "build looping seek buffer", failure);
    // Independently traverse the supplied regions: hear 80..99 once, then 0..99,
    // not 80..99 once followed forever by only 0..79.
    const auto* pcm = reinterpret_cast<const std::int16_t*>(buffer.pAudioData);
    std::vector<std::int16_t> heard;
    for (UINT32 i = 0; i < buffer.PlayLength; ++i) heard.push_back(pcm[buffer.PlayBegin + i]);
    for (int loop = 0; loop < 2; ++loop)
        for (UINT32 i = 0; i < buffer.LoopLength; ++i) heard.push_back(pcm[buffer.LoopBegin + i]);
    std::vector<std::int16_t> expected;
    for (int i = 80; i < 300; ++i) expected.push_back(static_cast<std::int16_t>(i % 100));
    r.check_eq(heard == expected, "seek preserves the complete song on every loop", "heard=" + std::to_string(heard.size()));
    r.check_invocation([&] { return engine.play(AudioBus::bgm, 0, stereo, "loop seek", 0, true, failure); }, "start looping track", failure);
    pump(engine, 0.02);
    r.check_invocation([&] { return engine.seek(AudioBus::bgm, 0, 0.8, failure); }, "XAudio2 accepts whole-song seek region", failure);
    pump_with_real_wait(engine, 0.4);
    const auto after_loop = engine.slot_state(AudioBus::bgm, 0);
    r.check_eq(after_loop.in_use && after_loop.position_seconds < 0.5,
        "real voice crosses the loop boundary", std::to_string(after_loop.position_seconds));
    stop_and_ignore(engine, AudioBus::bgm, 0);
}

void test_mix_and_fade(Registry& r, Fixture& fixture) {
    if (!fixture.ready) { r.skip("mix and fade require audio output"); return; }
    auto& engine = fixture.engine; Failure failure;
    stop_and_ignore(engine, AudioBus::bgm, 0); stop_and_ignore(engine, AudioBus::bgm, 1);
    reset_clock(); engine.update(g_clock.now());
    const auto pcm = make_pcm(8);
    r.check_invocation([&] { return engine.play(AudioBus::bgm, 0, pcm, "A", 0, true, failure); }, "start outgoing", failure);
    pump(engine, .01);
    r.check_invocation([&] { return engine.play(AudioBus::bgm, 1, pcm, "B", 1, true, failure); }, "prepare incoming", failure);
    r.check_invocation([&] { return engine.transition_bgm(0, 1, 1, true, failure); }, "crossfade two voices", failure);
    pump(engine, .5);
    const float a = engine.slot_state(AudioBus::bgm, 0).current_gain;
    const float b = engine.slot_state(AudioBus::bgm, 1).current_gain;
    r.check_eq(a > .69f && a < .72f && b > .69f && b < .72f && std::abs(a*a+b*b-1) < .01f,
        "equal-power midpoint", "old=" + std::to_string(a) + " new=" + std::to_string(b));
    r.check_invocation([&] { return engine.stop(AudioBus::bgm, 0, .1, failure); }, "shorten crossfade tail on user stop", failure);
    pump(engine, .15);
    r.check(!engine.slot_state(AudioBus::bgm, 0).in_use, "stop shortens an existing crossfade instead of leaving a tail");
    pump(engine, .45);
    r.check(engine.active_count(AudioBus::bgm) == 1, "crossfade reclaims outgoing voice");
    r.check_invocation([&] { return engine.pause(AudioBus::bgm, 1, failure, .6); }, "request fade pause", failure);
    pump(engine, .3);
    const auto middle = engine.slot_state(AudioBus::bgm, 1);
    r.check_eq(middle.paused && middle.current_gain > .45f && middle.current_gain < .55f,
        "pause intent is visible while output fades", std::to_string(middle.current_gain));
    r.check_invocation([&] { return engine.resume(AudioBus::bgm, 1, failure, .6); }, "reverse a pending pause", failure);
    pump(engine, .7);
    r.check(engine.slot_state(AudioBus::bgm, 1).playing && !engine.slot_state(AudioBus::bgm, 1).paused,
        "old pause deadline cannot stop resumed voice");
    r.check_invocation([&] { return engine.play(AudioBus::bgm, 0, pcm, "C", .4, true, failure); }, "prepare sequential incoming", failure);
    r.check_invocation([&] { return engine.transition_bgm(1, 0, .4, false, failure); }, "sequential transition", failure);
    pump(engine, .2);
    r.check(engine.slot_state(AudioBus::bgm, 0).current_gain == 0, "sequential incoming is silent before old fade completes");
    r.check_invocation([&] { return engine.pause(AudioBus::bgm, 0, failure, .6); }, "pause delayed incoming", failure);
    pump(engine, 1);
    r.check(engine.slot_state(AudioBus::bgm, 0).paused && !engine.slot_state(AudioBus::bgm, 0).playing,
        "delayed start cannot override pause");
    r.check_invocation([&] { return engine.resume(AudioBus::bgm, 0, failure, 0); }, "resume without fade", failure);
    r.check(engine.slot_state(AudioBus::bgm, 0).current_gain == 1, "fade off resumes immediately");
    r.check_invocation([&] { return engine.play(AudioBus::bgm, 0, pcm, "tail", .5, false, failure); }, "prepare natural tail", failure);
    pump(engine, .6);
    r.check_invocation([&] { return engine.seek(AudioBus::bgm, 0, 7.8, failure); }, "seek into natural tail", failure);
    pump(engine, .01);
    r.check(engine.slot_state(AudioBus::bgm, 0).stopping, "non-looping tail schedules fade");
    r.check_invocation([&] { return engine.set_loop(AudioBus::bgm, 0, true, failure); }, "enable loop during tail", failure);
    pump(engine, .8);
    r.check(engine.slot_state(AudioBus::bgm, 0).in_use && !engine.slot_state(AudioBus::bgm, 0).stopping,
        "loop cancels a previously scheduled natural-end fade");
    stop_and_ignore(engine, AudioBus::bgm, 0); stop_and_ignore(engine, AudioBus::bgm, 1);
}

}  // namespace

int wmain() {
    Registry r;
    Fixture fixture;
    static_cast<void>(fixture.initialize());   // 失败时后面各用例会自己 skip

    test_uninitialized_engine_fails_cleanly(r);
    test_initialize_reports_output_format(r, fixture);
    test_play_and_state(r, fixture);
    test_fade_in_reaches_full_gain(r, fixture);
    test_gain_survives_a_long_stall(r, fixture);
    test_pause_freezes_position(r, fixture);
    test_fade_out_then_release(r, fixture);
    test_buses_are_independent(r, fixture);
    test_slots_are_independent(r, fixture);
    test_looping_does_not_release(r, fixture);
    test_non_looping_releases_after_drain(r, fixture);
    test_find_free_slot_and_exhaustion(r, fixture);
    test_seek_changes_position(r, fixture);
    test_real_file_end_to_end(r, fixture);

    test_lifecycle_regressions(r, fixture);
    test_mix_and_fade(r, fixture);
    fixture.engine.shutdown();
    return r.finish("audio_engine_tests");
}
