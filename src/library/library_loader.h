#pragma once
#include <atomic>
#include <mutex>
#include <thread>
#include "library/library_scanner.h"
#include "audio/prepared_audio.h"

namespace musicboard {
struct LoadedTrack final {
    LibraryTrack track;
    std::string category;
    bool is_sfx = false;
    std::shared_ptr<const PreparedAudio> audio;
    Failure failure;
};
struct LibraryLoadProgress final {
    bool active = false;
    bool scanning = true;
    std::size_t done = 0, total = 0;
    std::uint64_t resident_bytes = 0;
    double fraction = 0.0;
    std::string current;
};
struct LibraryLoadResult final {
    std::filesystem::path root;
    std::vector<LoadedTrack> tracks;
    std::vector<std::string> bgm_categories, sfx_categories;
    std::vector<std::string> warnings;
    Failure failure;
    bool cancelled = false;
    double elapsed_seconds = 0.0;
    std::size_t cache_hits = 0;
};
class LibraryLoader final {
public:
    ~LibraryLoader();
    [[nodiscard]] bool start(const std::filesystem::path& root, Failure& failure, const std::filesystem::path& cache_root = {});
    void cancel() noexcept { cancelled_ = true; }
    void shutdown();
    [[nodiscard]] LibraryLoadProgress progress() const;
    [[nodiscard]] bool take(LibraryLoadResult& result);
private:
    void run(std::filesystem::path root);
    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> cancelled_{false};
    LibraryLoadProgress progress_;
    LibraryLoadResult result_;
    bool ready_ = false;
    std::filesystem::path cache_root_;
};
} // namespace musicboard
