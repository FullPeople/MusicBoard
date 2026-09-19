// 声部时间线测试
//
// 这个类是为了修掉一个具体的历史问题：Godot 版用"每帧 delta 累加"描述淡入淡出，
// 结果某一帧没跑到（比如 `tree.paused` 期间）音量就卡在半途、再也回不到正确值。
//
// 所以本文件的重点不是"某个时刻算得对不对"，而是**"算得对不对与查询方式无关"**：
//   · 掉帧、长时间不查询，恢复后第一次查询就要给出正确值
//   · 查询多少次、间隔多大，同一时刻的结果必须一致
// 这条性质有专门的用例（test_query_pattern_invariance）。

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "audio/voice_timeline.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

constexpr float kTolerance = 1e-4f;

[[nodiscard]] bool nearly_equal(float a, float b, float tolerance = kTolerance) {
    return std::fabs(a - b) <= tolerance;
}

void test_inactive_is_silent(Registry& r) {
    const VoiceTimeline timeline;
    r.check(timeline.gain_at(0.0) == 0.0f, "未激活时任何时刻增益都是 0");
    r.check(timeline.gain_at(1000.0) == 0.0f, "未激活时很远的时刻也是 0");
    r.check(timeline.is_finished(0.0), "未激活即视为已结束");
}

void test_no_fade_is_immediate(Registry& r) {
    VoiceTimeline timeline;
    timeline.begin(10.0, 0.0, 1.0f);
    r.check(nearly_equal(timeline.gain_at(10.0), 1.0f), "不淡入时起点即满增益");
    r.check(nearly_equal(timeline.gain_at(10.5), 1.0f), "不淡入时中途也是满增益");
    r.check(!timeline.is_finished(999.0), "没有停止请求时永远不算结束（循环播放靠这条）");
}

void test_fade_in_shape(Registry& r) {
    VoiceTimeline timeline;
    timeline.begin(100.0, 2.0, 1.0f);

    r.check(nearly_equal(timeline.gain_at(100.0), 0.0f), "淡入起点是 0");
    r.check(nearly_equal(timeline.gain_at(100.5), 0.25f), "淡入 1/4 处是 0.25");
    r.check(nearly_equal(timeline.gain_at(101.0), 0.5f), "淡入一半处是 0.5");
    r.check(nearly_equal(timeline.gain_at(101.5), 0.75f), "淡入 3/4 处是 0.75");
    r.check(nearly_equal(timeline.gain_at(102.0), 1.0f), "淡入结束时到满增益");
    r.check(nearly_equal(timeline.gain_at(200.0), 1.0f), "淡入结束后保持满增益");
    r.check(nearly_equal(timeline.gain_at(99.0), 0.0f), "起点之前是 0");
}

void test_fade_in_respects_target_gain(Registry& r) {
    VoiceTimeline timeline;
    timeline.begin(0.0, 4.0, 0.4f);   // 淡入到 0.4 而不是 1.0
    r.check(nearly_equal(timeline.gain_at(0.0), 0.0f), "目标 0.4 时起点仍是 0");
    r.check(nearly_equal(timeline.gain_at(2.0), 0.2f), "目标 0.4 时半途是 0.2");
    r.check(nearly_equal(timeline.gain_at(4.0), 0.4f), "目标 0.4 时终点是 0.4");
}

void test_fade_out_shape(Registry& r) {
    VoiceTimeline timeline;
    timeline.begin(50.0, 0.0, 1.0f);
    timeline.begin_stop(60.0, 1.0);

    r.check(nearly_equal(timeline.gain_at(60.0), 1.0f), "淡出起点仍是满增益");
    r.check(nearly_equal(timeline.gain_at(60.25), 0.75f), "淡出 1/4 处是 0.75");
    r.check(nearly_equal(timeline.gain_at(60.5), 0.5f), "淡出一半处是 0.5");
    r.check(nearly_equal(timeline.gain_at(61.0), 0.0f), "淡出结束时是 0");
    r.check(!timeline.is_finished(60.9), "淡出未跑完时不算结束");
    r.check(timeline.is_finished(61.0), "淡出跑完算结束");
    r.check(timeline.is_finished(62.0), "淡出结束后仍是结束");
}

void test_fade_out_from_mid_fade_in(Registry& r) {
    // 淡入还没跑完就被停：淡出必须从"当时那个值"开始降，而不是从满增益开始降，
    // 否则会出现"点了停止反而突然变响"。
    VoiceTimeline timeline;
    timeline.begin(0.0, 4.0, 1.0f);
    timeline.begin_stop(1.0, 2.0);   // 淡入到 0.25 时开始停

    r.check(nearly_equal(timeline.gain_at(1.0), 0.25f), "淡出起点接的是淡入中途那个值");
    r.check(nearly_equal(timeline.gain_at(2.0), 0.125f), "淡出一半处是当时值的一半");
    r.check(nearly_equal(timeline.gain_at(3.0), 0.0f), "淡出结束归零");
    r.check(timeline.gain_at(1.5) <= 0.25f, "淡出过程中不会超过起点值");
}

void test_repeated_stop_does_not_restart_fade(Registry& r) {
    // 重复调用 begin_stop 不能把淡出重新计时。
    // 若重新计时，表现为"点了停止但声音一直在" —— 因为淡出永远跑不完。
    VoiceTimeline timeline;
    timeline.begin(0.0, 0.0, 1.0f);
    timeline.begin_stop(10.0, 2.0);
    const float at_eleven = timeline.gain_at(11.0);

    timeline.begin_stop(11.0, 2.0);   // 又来一次
    r.check(nearly_equal(timeline.gain_at(11.0), at_eleven),
            "重复请求停止不会改变当前增益");
    r.check(timeline.is_finished(12.0), "重复请求停止不影响原定结束时刻");
}

void test_instant_stop(Registry& r) {
    VoiceTimeline timeline;
    timeline.begin(0.0, 0.0, 1.0f);
    timeline.begin_stop(5.0, 0.0);
    r.check(nearly_equal(timeline.gain_at(5.0), 0.0f), "零时长淡出即时刻归零");
    r.check(timeline.is_finished(5.0), "零时长淡出即时刻结束");
}

void test_elapsed_ignores_fade(Registry& r) {
    VoiceTimeline timeline;
    timeline.begin(1000.0, 1.0, 1.0f);
    r.check(nearly_equal(static_cast<float>(timeline.elapsed_at(1003.0)), 3.0f),
            "已播时长是 3 秒");
    timeline.begin_stop(1003.0, 1.0);
    r.check(nearly_equal(static_cast<float>(timeline.elapsed_at(1003.5)), 3.0f),
            "淡出期间已播时长冻结在停止那一刻（界面进度条靠这条）");
    r.check(nearly_equal(static_cast<float>(timeline.elapsed_at(2000.0)), 3.0f),
            "淡出结束后已播时长仍是停止那一刻的值");
}

// ---------------------------------------------------------------------------
// 这一条是本文件存在的理由
// ---------------------------------------------------------------------------
void test_query_pattern_invariance(Registry& r) {
    // 同一时刻，无论之前查询过多少次、间隔多大，结果必须一致。
    // 这正是"每帧 delta 累加"做不到的事：累加的结果取决于跑过多少帧。
    VoiceTimeline dense;
    dense.begin(0.0, 3.0, 1.0f);
    float last = 0.0f;
    for (int step = 0; step <= 3000; ++step) {   // 每 1 毫秒查一次
        last = dense.gain_at(static_cast<double>(step) / 1000.0);
    }

    VoiceTimeline sparse;
    sparse.begin(0.0, 3.0, 1.0f);
    const float single_query = sparse.gain_at(3.0);   // 只在最后查一次

    r.check(nearly_equal(last, 1.0f), "密集查询跑到 3 秒时是满增益");
    r.check(nearly_equal(single_query, 1.0f), "只查一次也是满增益");
    r.check(nearly_equal(last, single_query), "密集查询与单次查询结果一致");
}

void test_long_stall_recovers(Registry& r) {
    // 模拟"卡了很久没查询"：淡入早就该跑完了，恢复后第一次查询必须给出正确值，
    // 而不是停在半途。
    VoiceTimeline timeline;
    timeline.begin(0.0, 1.0, 1.0f);
    // 中途查一次，看到 0.5
    const float midway = timeline.gain_at(0.5);
    r.check(nearly_equal(midway, 0.5f), "卡顿前查到 0.5");

    // 然后整整 60 秒不查询（模拟 tree.paused 或掉帧）
    const float after_stall = timeline.gain_at(60.0);
    r.check(nearly_equal(after_stall, 1.0f),
            "长时间不查询后第一次查询直接给出正确值（不是停在半途）");
}

void test_negative_durations_are_safe(Registry& r) {
    VoiceTimeline timeline;
    timeline.begin(0.0, -5.0, 1.0f);
    r.check(nearly_equal(timeline.gain_at(0.0), 1.0f), "负的淡入时长按不淡入处理");

    VoiceTimeline other;
    other.begin(0.0, 1.0, 1.0f);
    other.begin_stop(1.0, -3.0);
    r.check(nearly_equal(other.gain_at(1.0), 0.0f), "负的淡出时长按立即停止处理");
    r.check(other.is_finished(1.0), "负的淡出时长立即结束");
}

void test_gain_is_clamped(Registry& r) {
    VoiceTimeline timeline;
    timeline.begin(0.0, 0.0, 5.0f);   // 越界的目标增益
    r.check(nearly_equal(timeline.gain_at(1.0), 1.0f), "超过 1 的目标增益被夹到 1");

    VoiceTimeline negative;
    negative.begin(0.0, 0.0, -2.0f);
    r.check(nearly_equal(negative.gain_at(1.0), 0.0f), "负的目标增益被夹到 0");
}

void test_reset(Registry& r) {
    VoiceTimeline timeline;
    timeline.begin(0.0, 1.0, 1.0f);
    timeline.begin_stop(1.0, 1.0);
    timeline.reset();
    r.check(!timeline.active, "reset 后不再激活");
    r.check(timeline.gain_at(2.0) == 0.0f, "reset 后增益为 0");
    r.check(timeline.is_finished(2.0), "reset 后视为已结束");
}

void test_monotonic_fade_in(Registry& r) {
    // 淡入必须单调不减。非单调意味着声音会"忽大忽小"。
    VoiceTimeline timeline;
    timeline.begin(0.0, 2.5, 1.0f);
    float previous = -1.0f;
    bool monotonic = true;
    std::string first_violation;
    for (int step = 0; step <= 500; ++step) {
        const double t = static_cast<double>(step) * 0.005;
        const float gain = timeline.gain_at(t);
        if (gain < previous - kTolerance) {
            monotonic = false;
            if (first_violation.empty()) {
                first_violation = "t=" + std::to_string(t) + " gain=" + std::to_string(gain) +
                                  " 小于前值 " + std::to_string(previous);
            }
        }
        previous = gain;
    }
    r.check_eq(monotonic, "淡入全程单调不减", first_violation);
}

void test_monotonic_fade_out(Registry& r) {
    VoiceTimeline timeline;
    timeline.begin(0.0, 0.0, 1.0f);
    timeline.begin_stop(10.0, 2.0);
    float previous = 2.0f;
    bool monotonic = true;
    std::string first_violation;
    for (int step = 0; step <= 400; ++step) {
        const double t = 10.0 + static_cast<double>(step) * 0.005;
        const float gain = timeline.gain_at(t);
        if (gain > previous + kTolerance) {
            monotonic = false;
            if (first_violation.empty()) {
                first_violation = "t=" + std::to_string(t) + " gain=" + std::to_string(gain) +
                                  " 大于前值 " + std::to_string(previous);
            }
        }
        previous = gain;
    }
    r.check_eq(monotonic, "淡出全程单调不增", first_violation);
}

}  // namespace

int main() {
    Registry r;
    test_inactive_is_silent(r);
    test_no_fade_is_immediate(r);
    test_fade_in_shape(r);
    test_fade_in_respects_target_gain(r);
    test_fade_out_shape(r);
    test_fade_out_from_mid_fade_in(r);
    test_repeated_stop_does_not_restart_fade(r);
    test_instant_stop(r);
    test_elapsed_ignores_fade(r);
    test_query_pattern_invariance(r);
    test_long_stall_recovers(r);
    test_negative_durations_are_safe(r);
    test_gain_is_clamped(r);
    test_reset(r);
    test_monotonic_fade_in(r);
    test_monotonic_fade_out(r);
    return r.finish("voice_timeline_tests");
}
