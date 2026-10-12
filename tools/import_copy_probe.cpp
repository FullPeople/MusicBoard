// Verify real cross-volume publication and cancellation in caller-owned fixtures.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include "library/import_transaction.h"

namespace {
std::size_t audio_files(const std::filesystem::path& directory) {
    std::error_code error;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error) && it->path().extension() == L".wav") ++count;
    return count;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 && argc != 5) return 2;
    std::atomic<bool> cancelled{false};
    std::uint64_t cancel_after = 0;
    if (argc == 5) {
        wchar_t* end = nullptr;
        cancel_after = std::wcstoull(argv[4], &end, 10);
        if (!end || *end || cancel_after == 0) return 2;
    }
    musicboard::ImportRequest request;
    request.staged_file = argv[1];
    request.staging_root = argv[2];
    request.target_category_dir = argv[3];
    request.cancelled = &cancelled;
    const auto baseline = audio_files(request.target_category_dir);
    std::size_t callbacks = 0;
    std::uint64_t copied = 0;
    bool early_publication = false;
    request.copy_progress = [&](std::uint64_t done, std::uint64_t) {
        ++callbacks;
        copied = done;
        early_publication |= audio_files(request.target_category_dir) != baseline;
        if (cancel_after && done >= cancel_after) cancelled = true;
    };
    musicboard::ImportResult result;
    musicboard::Failure failure;
    const auto began = GetTickCount64();
    const bool success = musicboard::import_staged_file(request, result, failure);
    const auto elapsed = GetTickCount64() - began;
    std::error_code error;
    const bool source_exists = std::filesystem::exists(request.staged_file, error);
    bool temporary_left = false;
    for (const auto& entry : std::filesystem::directory_iterator(request.target_category_dir, error))
        temporary_left |= entry.path().filename().native().starts_with(L".musicboard-import-");
    const bool passed = callbacks > 0 && !early_publication && !temporary_left &&
        (cancel_after ? !success && failure.code == musicboard::fail_code::cancelled && source_exists &&
            audio_files(request.target_category_dir) == baseline
            : success && !source_exists && audio_files(request.target_category_dir) == baseline + 1);
    std::printf("{\"Passed\":%s,\"Success\":%s,\"Cancelled\":%s,\"Callbacks\":%zu,"
        "\"CopiedBytes\":%llu,\"EarlyPublication\":%s,\"TemporaryLeft\":%s,\"Milliseconds\":%llu}\n",
        passed ? "true" : "false", success ? "true" : "false", cancelled ? "true" : "false",
        callbacks, static_cast<unsigned long long>(copied), early_publication ? "true" : "false",
        temporary_left ? "true" : "false", static_cast<unsigned long long>(elapsed));
    if (!passed) std::fprintf(stderr, "%s\n", failure.to_string().c_str());
    return passed ? 0 : 1;
}
