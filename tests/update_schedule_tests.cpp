#include "app/update_schedule.h"
#include "tests/test_harness.h"

using musicboard::app::UpdateSchedule;
using musicboard::test::Registry;
using namespace std::chrono_literals;

int main() {
    Registry r;
    const UpdateSchedule::Clock::time_point start{};
    const std::uint64_t stamp = 130000000000000000ULL;
    constexpr auto day_ticks = UpdateSchedule::check_interval_filetime;
    UpdateSchedule schedule(start);
    r.check(!schedule.take_probe(start + 9s), "startup leaves playback initialization alone");
    r.check(schedule.take_probe(start + 10s), "first background probe after ten seconds");
    r.check(!schedule.take_probe(start + 10s), "same frame cannot repeat a probe");
    r.check(!schedule.take_probe(start + 69s), "lightweight probe is limited to once a minute");
    r.check(schedule.take_probe(start + 70s), "background probing continues after startup");
    r.check(schedule.take_probe(start + 72h), "resume after days asleep performs one probe");
    r.check(schedule.next_probe() == start + 72h + 1min, "resume does not replay missed probes");
    r.check(!schedule.take_probe(start + 72h), "long sleep cannot produce a catch-up burst");

    UpdateSchedule fresh(start);
    r.check(fresh.background_due(start + 10s, stamp, std::nullopt), "missing marker permits first check");
    r.check(!fresh.background_due(start + 10s, stamp, stamp), "fresh marker suppresses network check");
    r.check(!fresh.background_due(start + 10s, stamp + day_ticks - 1, stamp), "persisted daily boundary is exact");
    r.check(fresh.background_due(start + 10s, stamp + day_ticks, stamp), "previous session becomes due after a day");
    r.check(fresh.background_due(start + 10s, stamp - 1, stamp), "future marker recovers after clock rollback");
    fresh.launched(start + 10s);
    r.check(!fresh.background_due(start + 11s, stamp + day_ticks * 30, stamp), "clock jump forward cannot launch repeatedly");
    r.check(!fresh.background_due(start + 23h, stamp - day_ticks * 30, stamp), "clock jump backward cannot launch repeatedly");
    r.check(!fresh.background_due(start + 24h + 9s, stamp + day_ticks, stamp), "steady daily boundary is exact");
    r.check(fresh.background_due(start + 24h + 10s, stamp + day_ticks, stamp), "running application rechecks after a day");
    r.check(fresh.background_due(start + 24h + 10s, stamp - day_ticks, stamp), "clock rollback does not permanently disable updates");
    r.check(!fresh.background_due(start + 24h + 10s, stamp, stamp), "new external marker retains cross-launch daily bound");

    UpdateSchedule retry(start);
    r.check(retry.take_probe(start + 10s), "initial failed launch starts with a probe");
    r.check(retry.take_probe(start + 70s) && retry.background_due(start + 70s, stamp, std::nullopt),
        "launch failure can retry later without consuming daily allowance");
    retry.launched(start + 70s);
    r.check(!retry.background_due(start + 80s, stamp, std::nullopt), "manual launch also postpones redundant automatic checks");
    return r.finish("update_schedule_tests");
}
