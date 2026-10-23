#include "BundleExtract.h"
#include <windows.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
void json_text(const std::string& text) {
    std::cout << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : text) {
        if (character == '"' || character == '\\') std::cout << '\\' << static_cast<char>(character);
        else if (character < 32) std::cout << "\\u00" << hex[character >> 4] << hex[character & 15];
        else std::cout << static_cast<char>(character);
    }
    std::cout << '"';
}
struct Mapping final {
    HANDLE file = INVALID_HANDLE_VALUE, handle = nullptr;
    const std::uint8_t* bytes = nullptr;
    ~Mapping() { if (bytes) UnmapViewOfFile(bytes); if (handle) CloseHandle(handle); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};
}

int wmain(int count, wchar_t** arguments) {
    if (count == 4 && std::wstring(arguments[1]) == L"--cleanup") {
        std::string error;
        const bool removed = fulltools::remove_owned_stage(arguments[2], arguments[3], error);
        std::cout << "{\"CleanupSucceeded\":" << (removed ? "true" : "false") << ",\"Error\":";
        json_text(error); std::cout << "}\n"; return removed ? 0 : 1;
    }
    if (count < 3) return 2;
    const std::filesystem::path input(arguments[1]), output(arguments[2]);
    fulltools::ExtractionOptions options;
    std::uint64_t cancel_after = std::numeric_limits<std::uint64_t>::max();
    for (int index = 3; index < count; ++index) {
        const std::wstring argument = arguments[index];
        if (argument == L"--cached-staging") options.flush_staged_files = false;
        else if (argument == L"--cancel-after" && index + 1 < count) {
            const wchar_t* value = arguments[++index]; wchar_t* end = nullptr; errno = 0;
            const auto parsed = std::wcstoull(value, &end, 10);
            if (!value[0] || value[0] == L'-' || errno == ERANGE || !end || *end) return 2;
            cancel_after = static_cast<std::uint64_t>(parsed);
        } else return 2;
    }
    Mapping mapping;
    mapping.file = CreateFileW(input.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    if (mapping.file == INVALID_HANDLE_VALUE || !GetFileSizeEx(mapping.file, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > std::numeric_limits<std::size_t>::max()) return 2;
    mapping.handle = CreateFileMappingW(mapping.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    mapping.bytes = mapping.handle ? static_cast<const std::uint8_t*>(MapViewOfFile(mapping.handle, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    if (!mapping.bytes) return 2;
    fulltools::ExtractionProgress progress;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> samples;
    progress.observer = [&](std::uint64_t done, std::uint64_t total) {
        if (samples.empty() || samples.back() != std::pair(done, total)) samples.emplace_back(done, total);
        if (done >= cancel_after) progress.cancelled.store(true);
    };
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    const bool extracted = fulltools::extract_bundle(mapping.bytes, static_cast<std::size_t>(size.QuadPart), output, progress, error, options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "{\"Extracted\":" << (extracted ? "true" : "false") << ",\"Error\":"; json_text(error);
    std::cout << ",\"ExtractedBytes\":" << progress.completed.load() << ",\"TotalBytes\":" << progress.total.load()
        << ",\"Files\":" << progress.files.load() << ",\"Cancelled\":" << (progress.cancelled.load() ? "true" : "false")
        << ",\"ElapsedMs\":" << elapsed << ",\"ManifestSha256\":"; json_text(progress.manifest_sha256);
    std::cout << ",\"ProgressSamples\":[";
    bool first = true;
    for (const auto& sample : samples) { if (!first) std::cout << ','; first = false; std::cout << '[' << sample.first << ',' << sample.second << ']'; }
    std::cout << "]}\n";
    return extracted ? 0 : 1;
}
