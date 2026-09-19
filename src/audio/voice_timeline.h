#pragma once

#include <cstdint>

namespace musicboard {

// ============================================================================
// 声部时间线
//
// 这个类存在的唯一理由：**淡入淡出必须由绝对时间算出，不能靠每帧累加。**
//
// Godot 版的原话是"靠每帧 delta 累加，打开文件夹对话框（tree.paused）时会卡在半途音量上"。
// 根因不是那个 pause，而是"用增量描述一个本该由时间决定的东西"——
// 只要有一帧没跑到，音量就永远回不到正确值。
//
// 这里反过来：调用方给一个绝对时间戳，gain_at() 直接算出那一刻该是多少。
// 于是：
//   · 掉帧、卡顿、被挂起 —— 恢复后第一次查询就回到正确值
//   · 查询多少次、间隔多大，结果都一样（下面 tests 里有专门的用例钉住这条）
//   · 暂停不需要任何特殊处理：时间不推进，gain 自然不变
//
// 时间单位是秒的 double。起点由调用方给（用 steady_clock 换算），
// 这样它不依赖任何全局状态，可以完全离线测。
// ============================================================================

enum class FadeCurve { linear, equal_power_in, equal_power_out };

struct VoiceTimeline final {
    bool active = false;

    // 播放起点与本段目标增益
    double started_at = 0.0;
    float target_gain = 1.0f;
    float initial_gain = 0.0f;
    FadeCurve curve = FadeCurve::linear;
    FadeCurve stop_curve = FadeCurve::linear;

    // 淡入：从 0 线性升到 target_gain
    double fade_in_seconds = 0.0;

    // 淡出：从当前值线性降到 0，降完即结束
    bool stopping = false;
    double fade_out_started_at = 0.0;
    double fade_out_seconds = 0.0;

    // 开始播放。fade_in <= 0 表示立即满音量。
    void begin(double now, double fade_in, float gain) noexcept;
    void begin_from(double now, double duration, float from, float to,
                    FadeCurve shape = FadeCurve::linear) noexcept;

    // 开始淡出。fade_out <= 0 表示立即停止。
    void begin_stop(double now, double fade_out, FadeCurve shape = FadeCurve::linear) noexcept;

    // 这一刻该用的增益。未激活时返回 0。
    [[nodiscard]] float gain_at(double now) const noexcept;

    // 是否已经彻底结束（淡出跑完，或从未激活）。
    [[nodiscard]] bool is_finished(double now) const noexcept;

    // 已经播了多久（淡出期间等于淡出开始时刻的已播时长）。
    [[nodiscard]] double elapsed_at(double now) const noexcept;

    void reset() noexcept { *this = VoiceTimeline{}; }
};

}  // namespace musicboard
