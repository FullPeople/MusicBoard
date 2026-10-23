#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <functional>

namespace fulltools {
struct ExtractionProgress final {
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> total{0};
    std::atomic<bool> cancelled{false};
    std::atomic<std::uint64_t> omitted_bytes{0};
    std::atomic<std::uint64_t> files{0};
    std::function<void(std::uint64_t, std::uint64_t)> observer;
    std::string manifest_sha256;
};
struct ExtractionOptions final {
    bool flush_staged_files = true;
};
[[nodiscard]] bool extract_bundle(const std::uint8_t* bytes, std::size_t length,
    const std::filesystem::path& destination, ExtractionProgress& progress, std::string& error,
    const ExtractionOptions& options = {});
[[nodiscard]] bool remove_owned_stage(const std::filesystem::path& destination,
    const std::filesystem::path& expected_parent, std::string& error);
}
