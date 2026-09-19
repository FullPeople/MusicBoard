#include "audio/voice_timeline.h"

#include <algorithm>
#include <cmath>

namespace musicboard {
namespace {

// 时长小于它一律当作 0 —— 避免 0 除，也避免极短的淡入淡出产生无意义的浮点噪声
constexpr double kMinimumAudibleDuration = 1e-4;

// "如果此刻不是在淡出，该是多少" —— 淡入与淡出共用同一套算法，
// 避免两处各写一遍导致行为不一致。
[[nodiscard]] float playing_gain_at(const VoiceTimeline& timeline, double now) noexcept {
    const double elapsed = now - timeline.started_at;
    if (elapsed <= 0.0) {
        return timeline.fade_in_seconds < kMinimumAudibleDuration ? timeline.target_gain : timeline.initial_gain;
    }
    if (timeline.fade_in_seconds < kMinimumAudibleDuration) {
        return timeline.target_gain;
    }
    if (elapsed >= timeline.fade_in_seconds) {
        return timeline.target_gain;
    }
    const double t = elapsed / timeline.fade_in_seconds;
    const double fraction = timeline.curve == FadeCurve::equal_power_in ? std::sin(t * 1.5707963267948966) : t;
    return static_cast<float>(timeline.initial_gain + (timeline.target_gain - timeline.initial_gain) * fraction);
}

}  // namespace

void VoiceTimeline::begin(double now, double fade_in, float gain) noexcept {
    begin_from(now, fade_in, 0.0f, gain);
}

void VoiceTimeline::begin_from(double now, double fade_in, float from, float gain, FadeCurve shape) noexcept {
    active = true;
    started_at = now;
    target_gain = std::clamp(gain, 0.0f, 1.0f);
    initial_gain = from;
    curve = shape;
    stop_curve = FadeCurve::linear;
    fade_in_seconds = std::max(0.0, fade_in);
    stopping = false;
    fade_out_started_at = 0.0;
    fade_out_seconds = 0.0;
}

void VoiceTimeline::begin_stop(double now, double fade_out, FadeCurve shape) noexcept {
    if (!active) {
        return;
    }
    // 已经在淡出就不重复启动 —— 否则每次调用都会把淡出重新计一遍，
    // 表现为"点了停止但声音一直在，因为淡出永远跑不完"。
    if (stopping) {
        return;
    }
    stopping = true;
    stop_curve = shape;
    fade_out_started_at = now;
    fade_out_seconds = std::max(0.0, fade_out);

    if (fade_out_seconds < kMinimumAudibleDuration) {
        // 立即停止：把淡出标记成"已跑完"
        fade_out_started_at = now - 1.0;
    }
}

float VoiceTimeline::gain_at(double now) const noexcept {
    if (!active) {
        return 0.0f;
    }

    if (stopping) {
        // 淡出从"停止那一刻本来该有的增益"线性降到 0。
        // 起点不能固定用 target_gain —— 那会让"淡入还没跑完就被停"的情况在停的瞬间突然变响。
        const double baseline = static_cast<double>(playing_gain_at(*this, fade_out_started_at));
        const double elapsed = now - fade_out_started_at;
        if (elapsed <= 0.0) {
            return static_cast<float>(baseline);
        }
        if (elapsed >= fade_out_seconds) {
            return 0.0f;
        }
        const double t = elapsed / fade_out_seconds;
        return static_cast<float>(baseline * (stop_curve == FadeCurve::equal_power_out ?
            std::cos(t * 1.5707963267948966) : 1.0 - t));
    }

    return playing_gain_at(*this, now);
}

bool VoiceTimeline::is_finished(double now) const noexcept {
    if (!active) {
        return true;
    }
    if (!stopping) {
        return false;   // 没有停止请求就永远不算结束（循环播放靠这个）
    }
    return (now - fade_out_started_at) >= fade_out_seconds;
}

double VoiceTimeline::elapsed_at(double now) const noexcept {
    const double reference = stopping ? fade_out_started_at : now;
    return std::max(0.0, reference - started_at);
}

}  // namespace musicboard
