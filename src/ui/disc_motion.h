#pragma once
#include <algorithm>
#include <cmath>

namespace musicboard::ui {
struct FadeMotion final {
    double stamp = 0; float from = 0, target = 0;
    [[nodiscard]] float at(double now) const noexcept {
        const auto t=static_cast<float>(std::clamp((now-stamp)/.16,0.0,1.0));
        return from+(target-from)*t*t*(3-2*t);
    }
    void set(bool enabled,double now) noexcept {
        const float next=enabled?1.0f:0.0f;
        if(next!=target){from=at(now);target=next;stamp=now;}
    }
    [[nodiscard]] bool moving(double now) const noexcept { return now < stamp+.16; }
};
// Both speed and angle are functions of absolute time, including interrupted ramps.
struct DiscMotion final {
    double stamp = 0, angle = 0, from_speed = 0, to_speed = 0;
    double arm_stamp = 0, arm_from = 0, arm_to = 0;
    static constexpr double ramp = 0.28;
    [[nodiscard]] double speed_at(double now) const noexcept {
        const double t = std::clamp((now - stamp) / ramp, 0.0, 1.0);
        return from_speed + (to_speed - from_speed) * t;
    }
    [[nodiscard]] double angle_at(double now) const noexcept {
        const double elapsed = std::max(0.0, now - stamp), t = std::min(elapsed, ramp);
        return angle + from_speed * t + (to_speed - from_speed) * t * t / (2 * ramp) +
            to_speed * std::max(0.0, elapsed - ramp);
    }
    [[nodiscard]] float arm_at(double now) const noexcept {
        const double t = std::clamp((now - arm_stamp) / ramp, 0.0, 1.0);
        return static_cast<float>(arm_from + (arm_to - arm_from) * t * t * (3.0 - 2.0 * t));
    }
    void set_playing(bool playing, double now) noexcept {
        const double target = playing ? 360.0 / 13.0 : 0.0;
        if (target == to_speed) return;
        angle = std::fmod(angle_at(now), 360.0); from_speed = speed_at(now); stamp = now; to_speed = target;
        arm_from = arm_at(now); arm_stamp = now; arm_to = playing ? 1.0 : 0.0;
    }
    [[nodiscard]] bool moving(double now) const noexcept {
        return to_speed > 0 || now < stamp + ramp;
    }
};
} // namespace musicboard::ui
