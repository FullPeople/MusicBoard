#include "audio/preload_cache.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include "core/file_bytes.h"
#include "core/path_utf8.h"
#include "core/sha256.h"

namespace musicboard {
namespace {
constexpr std::uint64_t magic = 0x32444c5250424dull;
void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) { out.push_back(static_cast<std::uint8_t>(value & 255)); value >>= 8; }
}
std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | bytes[offset + static_cast<std::size_t>(i)];
    return value;
}
bool read_cache(const std::filesystem::path& file, const std::filesystem::path& source,
    const AudioInfo& info, std::shared_ptr<const PreparedAudio>& result, Failure& failure) {
    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(file, bytes, failure, 128ull * 1048576)) return false;
    if (bytes.size() < 64 + 48 || read_u64(bytes, 0) != magic) {
        failure.set("cache.invalid", path_to_utf8(file) + " invalid preload cache header"); return false;
    }
    std::string digest;
    if (!sha256_hex(bytes.data(), bytes.size() - 64, digest, failure)) return false;
    if (std::memcmp(digest.data(), bytes.data() + bytes.size() - 64, 64) != 0) {
        failure.set("cache.invalid", path_to_utf8(file) + " preload checksum mismatch"); return false;
    }
    const auto rate = read_u64(bytes, 8), channels = read_u64(bytes, 16);
    const auto total = read_u64(bytes, 24), head_frames = read_u64(bytes, 32), bucket_count = read_u64(bytes, 40);
    if (rate != info.format.sample_rate || channels != info.format.channels || channels == 0 ||
        head_frames == 0 || head_frames > rate * 31 || total < head_frames || (bucket_count != 320 && bucket_count != 0) ||
        head_frames * channels * 2 + 48 + bucket_count * sizeof(float) + 64 != bytes.size()) {
        failure.set("cache.invalid", path_to_utf8(file) + " preload cache dimensions mismatch"); return false;
    }
    auto audio = std::make_shared<PreparedAudio>();
    audio->path = source; audio->head.format = info.format;
    audio->total_frames = total; audio->complete = head_frames == total;
    audio->head.duration_seconds = static_cast<double>(head_frames) / static_cast<double>(rate);
    audio->envelope.resize(static_cast<std::size_t>(bucket_count)); audio->head.samples.resize(static_cast<std::size_t>(head_frames * channels));
    if (bucket_count > 0) std::memcpy(audio->envelope.data(), bytes.data() + 48, static_cast<std::size_t>(bucket_count) * sizeof(float));
    std::memcpy(audio->head.samples.data(), bytes.data() + 48 + static_cast<std::size_t>(bucket_count) * sizeof(float), audio->head.samples.size() * 2);
    result = std::move(audio); return true;
}
bool write_cache(const std::filesystem::path& file, const PreparedAudio& audio, Failure& failure) {
    std::vector<std::uint8_t> bytes;
    for (const auto value : {magic, static_cast<std::uint64_t>(audio.head.format.sample_rate),
        static_cast<std::uint64_t>(audio.head.format.channels), audio.total_frames,
        static_cast<std::uint64_t>(audio.head.frame_count()), static_cast<std::uint64_t>(audio.envelope.size())}) append_u64(bytes, value);
    const auto* peaks = reinterpret_cast<const std::uint8_t*>(audio.envelope.data());
    if (!audio.envelope.empty()) bytes.insert(bytes.end(), peaks, peaks + audio.envelope.size() * sizeof(float));
    const auto* pcm = reinterpret_cast<const std::uint8_t*>(audio.head.samples.data());
    bytes.insert(bytes.end(), pcm, pcm + audio.head.samples.size() * 2);
    std::string digest;
    if (!sha256_hex(bytes.data(), bytes.size(), digest, failure)) return false;
    bytes.insert(bytes.end(), digest.begin(), digest.end());
    std::filesystem::path temporary = file;
    temporary += L"." + std::to_wstring(::GetCurrentProcessId()) + L"." + std::to_wstring(::GetCurrentThreadId()) + L".tmp";
    const HANDLE handle = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { failure.set("cache.write", path_to_utf8(temporary) + " CreateFile Win32=" + std::to_string(::GetLastError())); return false; }
    DWORD written = 0;
    const bool written_ok = ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE && written == bytes.size();
    const DWORD error = written_ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(handle);
    // This is regenerable cache data. Atomic replacement and the read checksum
    // suffice; forcing a physical disk flush per track stalls cold startup.
    if (!written_ok || !::MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        failure.set("cache.write", path_to_utf8(file) + " publish cache Win32=" + std::to_string(written_ok ? ::GetLastError() : error));
        ::DeleteFileW(temporary.c_str()); return false;
    }
    return true;
}
}
bool prepare_audio_cached(const std::filesystem::path& path, const AudioInfo& info,
    const std::filesystem::path& cache_root, std::shared_ptr<const PreparedAudio>& prepared,
    bool& cache_hit, std::string& cache_note, Failure& failure,
    const std::atomic<bool>* cancelled, const std::function<void(double)>& progress) {
    cache_hit = false; cache_note.clear(); prepared.reset();
    if (cancelled && *cancelled) { failure.set("audio.cancelled", path_to_utf8(path) + " preload cancelled"); return false; }
    if (cache_root.empty()) return prepare_audio(path, info, prepared, failure, cancelled, progress);
    // Source revision checks must not read gigabytes of compressed audio on
    // every start. Payload checksums still validate every byte of the cache.
    std::error_code revision_error;
    const auto size = std::filesystem::file_size(path, revision_error);
    if (revision_error) { failure.set("cache.source", path_to_utf8(path) + " " + revision_error.message()); return false; }
    const auto modified = std::filesystem::last_write_time(path, revision_error);
    if (revision_error) { failure.set("cache.source", path_to_utf8(path) + " " + revision_error.message()); return false; }
    const auto absolute = std::filesystem::absolute(path, revision_error);
    if (revision_error) { failure.set("cache.source", path_to_utf8(path) + " " + revision_error.message()); return false; }
    const std::string revision = "prefix2-v4:" + path_to_utf8(absolute.lexically_normal()) + ":" +
        std::to_string(size) + ":" + std::to_string(modified.time_since_epoch().count());
    std::string digest;
    if (!sha256_hex(reinterpret_cast<const std::uint8_t*>(revision.data()), revision.size(), digest, failure)) return false;
    std::filesystem::path name;
    if (!path_from_utf8(digest + ".mbpcm", name, failure)) return false;
    const auto file = cache_root / name;
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) { failure.set("cache.read", path_to_utf8(file) + " " + error.message()); return false; }
    Failure cache_failure;
    if (exists) {
        if (read_cache(file, path, info, prepared, cache_failure)) { cache_hit = true; return true; }
        cache_note = "缓存无效，已重新准备: " + cache_failure.to_string();
    }
    if (!prepare_audio(path, info, prepared, failure, cancelled, progress)) return false;
    const auto current_modified = std::filesystem::last_write_time(path, revision_error);
    if (revision_error || current_modified != modified) {
        prepared.reset();
        failure.set("cache.source_changed", path_to_utf8(path) + " changed while preparing; reload the library"); return false;
    }
    std::filesystem::create_directories(cache_root, error);
    cache_failure.clear();
    if (error) cache_note += " 缓存目录写入失败: " + path_to_utf8(cache_root) + " " + error.message();
    else if (!write_cache(file, *prepared, cache_failure)) cache_note += " 缓存保存失败: " + cache_failure.to_string();
    return true;
}
} // namespace musicboard
