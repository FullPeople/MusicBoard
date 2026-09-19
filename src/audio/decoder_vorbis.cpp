#include <windows.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "audio/decoder_backends.h"
#include "core/path_utf8.h"

// stb_vorbis 以单文件形式内置。刻意定义 STB_VORBIS_NO_STDIO：
// 它的文件 API 走 fopen + 窄字符路径，在中文文件名上必然失败。
// 本项目只走内存解码，让这个坑在结构上不可能发生。
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

namespace musicboard::detail {
namespace {

// Keep the memory API (no narrow paths), but map the compressed file instead
// of eagerly copying it. Probe/prefix/seek touch only the pages they need.
struct VorbisSource final {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const unsigned char* bytes = nullptr;
    stb_vorbis* decoder = nullptr;
    ~VorbisSource() {
        if (decoder) stb_vorbis_close(decoder);
        if (bytes) ::UnmapViewOfFile(bytes);
        if (mapping) ::CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
    }

    [[nodiscard]] bool open(const std::filesystem::path& path, Failure& failure) {
        const auto failed = [&](const char* operation) {
            failure.set(fail_code::file_open_failed, path_to_utf8(path) + " " + operation +
                " Win32=" + std::to_string(::GetLastError())); return false;
        };
        file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return failed("CreateFileW");
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size)) return failed("GetFileSizeEx");
        if (size.QuadPart <= 0 || size.QuadPart > std::numeric_limits<int>::max()) {
            failure.set(fail_code::decode_failed, path_to_utf8(path) + " unsupported Vorbis size=" + std::to_string(size.QuadPart));
            return false;
        }
        mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) return failed("CreateFileMappingW");
        bytes = static_cast<const unsigned char*>(::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!bytes) return failed("MapViewOfFile");
        int error = 0;
        decoder = stb_vorbis_open_memory(bytes, static_cast<int>(size.QuadPart), &error, nullptr);
        if (decoder == nullptr) {
            failure.set(fail_code::decode_failed,
                        path_to_utf8(path) + " stb_vorbis 打开失败，错误码 " + std::to_string(error));
            return false;
        }
        return true;
    }
};

[[nodiscard]] AudioFormat format_of(const stb_vorbis_info& info) {
    AudioFormat format;
    format.sample_rate = info.sample_rate;
    format.channels = static_cast<std::uint16_t>(info.channels);
    format.bits_per_sample = 16;   // stb_vorbis 统一输出 16 位
    return format;
}

}  // namespace

bool ogg_probe(const std::filesystem::path& path, AudioInfo& info, Failure& failure) {
    VorbisSource source;
    if (!source.open(path, failure)) {
        return false;
    }
    const stb_vorbis_info vorbis_info = stb_vorbis_get_info(source.decoder);
    info.format = format_of(vorbis_info);

    // 时长：stb_vorbis 对可 seek 的输入会跳到末尾取总样本数，不必整段解码。
    // 内存输入是可 seek 的，所以这一步很便宜。
    const unsigned int total_samples = stb_vorbis_stream_length_in_samples(source.decoder);
    if (total_samples == 0) {
        failure.set(fail_code::decode_failed, path_to_utf8(path) + " stb_vorbis 取不到样本总数");
        return false;
    }
    info.duration_seconds =
        static_cast<double>(total_samples) / static_cast<double>(info.format.sample_rate);
    return true;
}

bool ogg_read_chunks(const std::filesystem::path& path, AudioChunkConsumer& consumer,
                      Failure& failure, const std::atomic<bool>* cancelled, double start_seconds) {
    VorbisSource source;
    if (!source.open(path, failure)) return false;
    const AudioFormat format = format_of(stb_vorbis_get_info(source.decoder));
    if (format.channels == 0 || format.sample_rate == 0) {
        failure.set(fail_code::decode_failed, path_to_utf8(path) + " invalid Vorbis format");
        return false;
    }
    if (start_seconds > 0.0 && !stb_vorbis_seek(source.decoder,
            static_cast<unsigned int>(start_seconds * format.sample_rate))) {
        failure.set(fail_code::decode_failed, path_to_utf8(path) + " Vorbis seek failed"); return false;
    }
    constexpr int frames_per_chunk = 4096;
    std::vector<std::int16_t> chunk(static_cast<std::size_t>(frames_per_chunk) * format.channels);
    std::size_t total_frames = 0;
    for (;;) {
        if (cancelled && cancelled->load()) {
            failure.set("audio.cancelled", path_to_utf8(path) + " decoding cancelled");
            return false;
        }
        const int frames = stb_vorbis_get_samples_short_interleaved(source.decoder,
            format.channels, chunk.data(), static_cast<int>(chunk.size()));
        const int error = stb_vorbis_get_error(source.decoder);
        if (frames < 0 || error != VORBIS__no_error) {
            failure.set(fail_code::decode_failed, path_to_utf8(path) + " stb_vorbis error=" + std::to_string(error));
            return false;
        }
        if (frames == 0) break;
        if (!consumer.consume(format, chunk.data(), static_cast<std::size_t>(frames), failure)) return false;
        total_frames += static_cast<std::size_t>(frames);
        if (consumer.finished()) break;
    }
    if (total_frames == 0) {
        failure.set(fail_code::decode_failed, path_to_utf8(path) + " decoded zero frames");
        return false;
    }
    return true;
}

}  // namespace musicboard::detail
