#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace musicboard::app {

// No I/O: the caller reads the existing updater marker on its worker thread.
class UpdateSchedule final {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto initial_delay = std::chrono::seconds(10);
    static constexpr auto probe_interval = std::chrono::minutes(1);
    static constexpr auto check_interval = std::chrono::hours(24);
    static constexpr std::uint64_t check_interval_filetime = 24ULL * 60 * 60 * 10000000;

    explicit UpdateSchedule(Clock::time_point started) noexcept
        : next_probe_(started + initial_delay) {}

    [[nodiscard]] Clock::time_point next_probe() const noexcept { return next_probe_; }

    [[nodiscard]] bool take_probe(Clock::time_point now) noexcept {
        if (now < next_probe_) return false;
        // Resume from sleep with one probe, without replaying missed minutes.
        next_probe_ = now + probe_interval;
        return true;
    }

    [[nodiscard]] bool background_due(Clock::time_point now, std::uint64_t utc_filetime,
                                      std::optional<std::uint64_t> previous) const noexcept {
        // A wall-clock adjustment cannot cause repeated launches in this session.
        if (last_launch_ && now - *last_launch_ < check_interval) return false;
        // Match the signed updater's cross-launch marker, including clock rollback.
        return !previous || utc_filetime < *previous ||
            utc_filetime - *previous >= check_interval_filetime;
    }

    void launched(Clock::time_point now) noexcept { last_launch_ = now; }

private:
    Clock::time_point next_probe_;
    std::optional<Clock::time_point> last_launch_;
};

} // namespace musicboard::app
