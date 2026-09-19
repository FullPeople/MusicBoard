// 波形包络测试
//
// 全是纯函数，所以可以完全离线穷举 —— 不需要任何真实音频素材。
// 这里钉住的是"墙上的波形和耳朵听到的对得上"这件事的前提：
//   · 静音就是一条直线（不许伪造底噪、不许出现 NaN）
//   · 归一化按全局峰值（录得轻的素材也要看得出形状）
//   · 每个桶真的覆盖到了对应区间的样本（不是错位一格）
//   · 立体声取两声道里更大的那个（左声道轻右声道响不能被漏掉）
//   · 桶数比帧数还多时不崩、不除以 0

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/waveform.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

constexpr std::int16_t kFullScale = 32767;

std::vector<std::int16_t> constant_samples(std::size_t frames, std::size_t channels,
                                           std::int16_t value) {
    return std::vector<std::int16_t>(frames * channels, value);
}

// 一段正弦（单声道）
std::vector<std::int16_t> sine_samples(std::size_t frames, double amplitude) {
    std::vector<std::int16_t> samples(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double phase = 2.0 * 3.14159265358979 * static_cast<double>(i) / 32.0;
        samples[i] = static_cast<std::int16_t>(std::sin(phase) * amplitude * kFullScale);
    }
    return samples;
}

bool all_zero(const std::vector<float>& values) {
    for (const float value : values) {
        if (value != 0.0f) {
            return false;
        }
    }
    return true;
}

bool all_finite(const std::vector<float>& values) {
    for (const float value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    Registry registry;
    Failure failure;

    // =======================================================================
    // 参数校验
    // =======================================================================
    {
        std::vector<std::int16_t> samples = constant_samples(100, 1, 1000);
        Failure local;
        registry.check(compute_waveform_envelope(samples.data(), 100, 1, 0, local).empty() &&
                           local.code == fail_code::invalid_argument,
                       "桶数为 0 → 明确失败");
        Failure local2;
        registry.check(compute_waveform_envelope(samples.data(), 100, 0, 10, local2).empty() &&
                           local2.code == fail_code::invalid_argument,
                       "声道数为 0 → 明确失败");
    }

    // =======================================================================
    // 静音
    // =======================================================================
    {
        const std::size_t frames = 1000;
        std::vector<std::int16_t> samples = constant_samples(frames, 1, 0);
        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(samples.data(), frames, 1, 64, local);

        registry.check(local.empty(), "全静音不算失败");
        registry.check(envelope.size() == 64, "桶数正确");
        registry.check(all_zero(envelope), "静音是一条直线（不伪造底噪）");
        registry.check(all_finite(envelope), "静音不产生 NaN 或 inf");
    }
    {
        // 零帧：合法输入，给一条平坦的线
        const std::vector<std::int16_t> empty;
        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(empty.data(), 0, 1, 32, local);
        registry.check(local.empty() && envelope.size() == 32, "零帧仍然给出正确桶数的包络");
        registry.check(all_zero(envelope), "零帧的包络全 0");
    }
    {
        failure.clear();
        const std::vector<float> envelope =
            compute_waveform_envelope(nullptr, 0, 2, 16, failure);
        registry.check(envelope.size() == 16 && all_zero(envelope), "空指针 + 零帧不崩");
    }

    // =======================================================================
    // 恒定幅值：归一化之后应该是全 1
    // =======================================================================
    {
        const std::size_t frames = 500;
        std::vector<std::int16_t> samples = constant_samples(frames, 1, 8000);
        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(samples.data(), frames, 1, 32, local);

        bool all_one = true;
        for (const float value : envelope) {
            if (std::fabs(value - 1.0f) > 1e-6f) {
                all_one = false;
            }
        }
        registry.check(all_one, "恒定幅值归一化之后全是 1");
    }

    // =======================================================================
    // 归一化按全局峰值 —— 录得轻的素材也要看得出形状
    // =======================================================================
    {
        // 峰值只有满刻度的 10%
        std::vector<std::int16_t> samples = sine_samples(1024, 0.10);
        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(samples.data(), samples.size(), 1, 32, local);

        float maximum = 0.0f;
        for (const float value : envelope) {
            maximum = std::max(maximum, value);
        }
        registry.check_eq(std::fabs(maximum - 1.0f) < 1e-5f,
                          "峰值 10% 满刻度的素材归一化后最大值为 1（不贴在中间）",
                          std::to_string(maximum));
    }
    {
        // 反过来：这是刻意的取舍。整段都很轻但有一个尖峰时，
        // 归一化之后其余部分会显得很小 —— 那是**真实**的动态范围，
        // 不该被压平。这条断言把这个取舍写下来，免得以后被"顺手优化"掉。
        std::vector<std::int16_t> samples = constant_samples(1000, 1, 100);
        samples[500] = kFullScale;   // 一个满刻度尖峰
        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(samples.data(), 1000, 1, 10, local);

        float maximum = 0.0f;
        for (const float value : envelope) {
            maximum = std::max(maximum, value);
        }
        registry.check(std::fabs(maximum - 1.0f) < 1e-5f, "尖峰被归一化到 1");
        // 第 5 桶含尖峰；第 0 桶只有幅值 100 的样本
        registry.check(envelope.front() < 0.02f,
                       "其余部分保持很低的读数（动态范围没有被压平）");
    }

    // =======================================================================
    // 桶与样本区间的对应关系 —— 错位一格是这类代码最常见的错
    // =======================================================================
    {
        // 前半段静音、后半段满刻度：桶数 10 时，前 5 桶应该接近 0、后 5 桶应该是 1
        const std::size_t frames = 1000;
        std::vector<std::int16_t> samples(frames, 0);
        for (std::size_t i = frames / 2; i < frames; ++i) {
            samples[i] = kFullScale;
        }
        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(samples.data(), frames, 1, 10, local);

        registry.check(envelope.size() == 10, "桶数正确");
        bool first_half_silent = true;
        bool second_half_loud = true;
        for (std::size_t i = 0; i < 5; ++i) {
            if (envelope[i] > 0.01f) {
                first_half_silent = false;
            }
        }
        for (std::size_t i = 5; i < 10; ++i) {
            if (envelope[i] < 0.99f) {
                second_half_loud = false;
            }
        }
        registry.check(first_half_silent, "前半段静音 → 前 5 桶为 0（桶与区间对齐）");
        registry.check(second_half_loud, "后半段满刻度 → 后 5 桶为 1");
    }
    {
        // 一个单点尖峰落在第 k 桶，那个桶必须被点亮，别的桶不该
        const std::size_t frames = 1000;
        for (const std::size_t spike_frame : {std::size_t{50}, std::size_t{500},
                                              std::size_t{950}}) {
            std::vector<std::int16_t> samples(frames, 0);
            samples[spike_frame] = kFullScale;
            Failure local;
            const std::vector<float> envelope =
                compute_waveform_envelope(samples.data(), frames, 1, 10, local);

            const std::size_t expected_bucket = spike_frame * 10 / frames;
            registry.check_eq(std::fabs(envelope[expected_bucket] - 1.0f) < 1e-5f,
                              "尖峰落在第 " + std::to_string(spike_frame) + " 帧 → 第 " +
                                  std::to_string(expected_bucket) + " 桶被点亮",
                              std::to_string(envelope[expected_bucket]));

            int lit = 0;
            for (const float value : envelope) {
                if (value > 0.5f) {
                    ++lit;
                }
            }
            registry.check_eq(lit == 1, "只有一个桶被点亮（单点尖峰不会被扩散）",
                              "实际 " + std::to_string(lit));
        }
    }

    // =======================================================================
    // 所有帧都被扫到：末尾的尾巴不能被丢掉
    // =======================================================================
    {
        // 帧数 1000、桶数 3 —— 1000/3 除不尽。最后 1 帧必须属于最后一桶。
        std::vector<std::int16_t> samples(1000, 0);
        samples[999] = kFullScale;   // 最后一帧
        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(samples.data(), 1000, 1, 3, local);
        registry.check(envelope.back() > 0.99f, "最后一帧属于最后一桶（整数除法不丢尾巴）");
    }
    {
        // 桶数比帧数还多：不应该崩，也不应该除以 0
        std::vector<std::int16_t> samples = constant_samples(3, 1, kFullScale);
        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(samples.data(), 3, 1, 100, local);
        registry.check(envelope.size() == 100 && all_finite(envelope),
                       "桶数远多于帧数时不崩、不产生 NaN");
        registry.check(local.empty(), "桶数多于帧数不算失败（只是分辨率过剩）");
    }

    // =======================================================================
    // 立体声：取两声道里更大的那个
    // =======================================================================
    {
        // 左声道静音、右声道满刻度 —— 必须读出满刻度，不能被左声道糊弄过去
        const std::size_t frames = 500;
        std::vector<std::int16_t> samples(frames * 2, 0);
        for (std::size_t i = 0; i < frames; ++i) {
            samples[i * 2 + 1] = kFullScale;   // 右声道
        }
        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(samples.data(), frames, 2, 16, local);
        registry.check(envelope.front() > 0.99f, "只有右声道有声音时仍然读到满幅");

        // 反过来
        std::vector<std::int16_t> left_only(frames * 2, 0);
        for (std::size_t i = 0; i < frames; ++i) {
            left_only[i * 2] = kFullScale;
        }
        Failure local2;
        const std::vector<float> envelope2 =
            compute_waveform_envelope(left_only.data(), frames, 2, 16, local2);
        registry.check(envelope2.front() > 0.99f, "只有左声道有声音时同样读到满幅");
    }
    {
        // -32768 的绝对值是 32768（超出 int16 正范围）—— 不能算出负数或溢出
        const std::size_t frames = 100;
        std::vector<std::int16_t> samples(frames, -32768);
        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(samples.data(), frames, 1, 8, local);
        registry.check(all_finite(envelope), "-32768 不产生异常值");
        bool all_one = true;
        for (const float value : envelope) {
            if (std::fabs(value - 1.0f) > 1e-6f) {
                all_one = false;
            }
        }
        registry.check(all_one, "-32768 归一化之后是 1（不是负数）");
    }

    // =======================================================================
    // amplitude_at：悬浮球辉光用的取值
    // =======================================================================
    {
        const std::vector<float> envelope = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
        registry.check(amplitude_at(envelope, 0.0) == 0.0f, "progress 0 → 第 0 桶");
        registry.check(amplitude_at(envelope, 1.0) == 1.0f, "progress 1 → 最后一桶");
        registry.check(amplitude_at(envelope, 0.5) == 0.5f, "progress 0.5 → 中间那桶");
        registry.check(amplitude_at(envelope, -1.0) == 0.0f, "progress 负数被夹到 0");
        registry.check(amplitude_at(envelope, 2.0) == 1.0f, "progress 超过 1 被夹到 1");
        registry.check(amplitude_at(envelope, 0.99) == 1.0f, "接近 1 时落在最后一桶");
    }
    registry.check(amplitude_at({}, 0.5) == 0.0f, "空包络返回 0（没在播就不发光）");
    {
        const std::vector<float> single = {0.7f};
        registry.check(amplitude_at(single, 0.0) == 0.7f &&
                           amplitude_at(single, 1.0) == 0.7f,
                       "只有一个桶时任意进度都取到它（不会越界）");
    }

    // =======================================================================
    // DecodedAudio 重载
    // =======================================================================
    {
        DecodedAudio audio;
        audio.format.channels = 2;
        audio.format.sample_rate = 44100;
        audio.samples = constant_samples(200, 2, kFullScale);
        audio.duration_seconds = 200.0 / 44100.0;

        Failure local;
        const std::vector<float> envelope =
            compute_waveform_envelope(audio, kDefaultWaveformBuckets, local);
        registry.check(local.empty() && envelope.size() == kDefaultWaveformBuckets,
                       "从 DecodedAudio 算包络：默认桶数正确");
        registry.check(audio.frame_count() == 200, "帧数按声道数折算正确");
        registry.check(!all_zero(envelope), "有声音的音频不会得到全 0 的包络");
    }
    {
        // 声道数为 0 的 DecodedAudio 只可能来自解码器的 bug。
        // 这里**明确失败**而不是画一条平坦的线 —— 后者会让"解码坏了"
        // 伪装成"这首歌是静音"，而那正是本项目要避免的失败方式。
        DecodedAudio audio;
        audio.format.channels = 0;
        audio.samples = constant_samples(100, 1, kFullScale);
        Failure local;
        const std::vector<float> envelope = compute_waveform_envelope(audio, 16, local);
        registry.check(envelope.empty() && local.code == fail_code::invalid_argument,
                       "声道数为 0 → 明确报错（不伪装成静音）");
    }

    return registry.finish("waveform_tests");
}
