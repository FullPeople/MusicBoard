#include "library/library_loader.h"
#include "audio/preload_cache.h"
#include <windows.h>
#include <algorithm>
#include "core/path_utf8.h"

namespace musicboard {
LibraryLoader::~LibraryLoader() { shutdown(); }
void LibraryLoader::shutdown() { cancel(); if (thread_.joinable()) thread_.join(); }
bool LibraryLoader::start(const std::filesystem::path& root, Failure& failure, const std::filesystem::path& cache_root) {
    if (root.empty()) { failure.set(fail_code::invalid_argument, "Empty library preload root"); return false; }
    shutdown();
    cache_root_ = cache_root;
    {
        const std::lock_guard lock(mutex_);
        result_ = {};
        ready_ = false;
        progress_ = {};
        progress_.active = true;
        cancelled_ = false;
    }
    thread_ = std::thread([this, root] { run(root); });
    return true;
}
LibraryLoadProgress LibraryLoader::progress() const { const std::lock_guard lock(mutex_); return progress_; }
bool LibraryLoader::take(LibraryLoadResult& result) {
    const std::lock_guard lock(mutex_);
    if (!ready_) return false;
    result = std::move(result_);
    result.cancelled = result.cancelled || cancelled_.load();
    ready_ = false;
    return true;
}
void LibraryLoader::run(std::filesystem::path root) {
    const auto started = std::chrono::steady_clock::now();
    LibraryLoadResult result;
    result.root = root;
    ScanOptions options;
    options.root = root;
    options.should_cancel = [this] { return cancelled_.load(); };
    options.on_progress = [this](std::size_t done, std::size_t total, const std::filesystem::path& path) {
        const std::lock_guard lock(mutex_);
        progress_.done = done; progress_.total = total;
        progress_.fraction = total == 0 ? 0.0 : 0.1 * static_cast<double>(done) / static_cast<double>(total);
        progress_.current = path_to_utf8(path.filename());
    };
    LibrarySnapshot snapshot;
    if (scan_library(options, snapshot, result.failure)) {
        result.warnings = std::move(snapshot.warnings);
        const auto append = [&result](const auto& categories, bool sfx) {
            for (const auto& category : categories) for (const auto& track : category.tracks)
                result.tracks.push_back(LoadedTrack{track, category.name, sfx, {}, {}});
        };
        append(snapshot.bgm, false); append(snapshot.sfx, true);
        for (const auto& category : snapshot.bgm) result.bgm_categories.push_back(category.name);
        for (const auto& category : snapshot.sfx) result.sfx_categories.push_back(category.name);
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (!::GlobalMemoryStatusEx(&memory)) {
            result.failure.set(fail_code::platform_api_failed, "Preload memory query Win32=" + std::to_string(::GetLastError()));
        } else {
            const auto budget = std::min<std::uint64_t>(768ull * 1024 * 1024, memory.ullAvailPhys / 3);
            std::uint64_t predicted = 0;
            for (const auto& item : result.tracks) predicted += static_cast<std::uint64_t>(
                (item.track.duration_seconds <= 30.0 ? item.track.duration_seconds : preload_head_seconds) *
                item.track.format.sample_rate * item.track.format.channels * sizeof(std::int16_t));
            if (predicted > budget) result.failure.set("library.preload_memory", "预加载需要 " +
                std::to_string(predicted / 1048576) + " MB，可用预算 " + std::to_string(budget / 1048576) +
                " MB；请释放内存或选择较小曲库：" + path_to_utf8(root));
        }
        if (result.failure.empty()) {
            std::atomic<std::size_t> next{0};
            std::vector<double> fractions(result.tracks.size(), 0.0);
            std::uint64_t bytes = 0;
            {
                const std::lock_guard lock(mutex_);
                progress_.scanning = false; progress_.done = 0; progress_.total = result.tracks.size();
            }
            const auto work = [&] {
                while (!cancelled_) {
                    const auto i = next.fetch_add(1);
                    if (i >= result.tracks.size()) return;
                    auto& item = result.tracks[i];
                    {
                        const std::lock_guard lock(mutex_); progress_.current = item.track.name;
                    }
                    AudioInfo info{item.track.container, item.track.format, item.track.duration_seconds};
                    bool cache_hit = false;
                    std::string cache_note;
                    double last_progress = -1.0;
                    const bool success = prepare_audio_cached(item.track.path, info, cache_root_, item.audio,
                        cache_hit, cache_note, item.failure, &cancelled_, [&, i](double fraction) {
                            if (fraction - last_progress < 0.01) return;
                            last_progress = fraction;
                            const std::lock_guard lock(mutex_);
                            fractions[i] = fraction;
                            double sum = 0.0;
                            for (const double value : fractions) sum += value;
                            progress_.fraction = 0.1 + 0.9 * sum / static_cast<double>(result.tracks.size());
                        });
                    const std::lock_guard lock(mutex_);
                    if (!success && !cancelled_) result.warnings.push_back(item.failure.to_string());
                    if (!cache_note.empty()) result.warnings.push_back(cache_note);
                    if (cache_hit) ++result.cache_hits;
                    if (item.audio) bytes += item.audio->head.samples.size() * sizeof(std::int16_t);
                    fractions[i] = 1.0;
                    double sum = 0.0;
                    for (const double value : fractions) sum += value;
                    progress_.fraction = 0.1 + 0.9 * sum / static_cast<double>(result.tracks.size());
                    ++progress_.done; progress_.resident_bytes = bytes;
                }
            };
            std::thread worker1(work), worker2(work);
            work();
            worker1.join(); worker2.join();
        }
    }
    result.cancelled = cancelled_;
    result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const std::lock_guard lock(mutex_);
    progress_.active = false;
    if (result.failure.empty() && !result.cancelled) progress_.fraction = 1.0;
    result_ = std::move(result);
    ready_ = true;
}
} // namespace musicboard
