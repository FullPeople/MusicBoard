#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "audio/decoder_backends.h"
#include <algorithm>
#include "core/path_utf8.h"
#include "core/com_apartment.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace musicboard::detail {
namespace {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Media Foundation 的进程级生命周期
//
// MFStartup 必须与 MFShutdown 配对，且只能初始化一次。用函数内静态量保证
// 只做一次；这个函数本身是线程安全的（C++11 起的静态局部变量初始化保证）。
// ---------------------------------------------------------------------------
[[nodiscard]] bool ensure_media_foundation(Failure& failure) {
    struct MediaFoundationLifetime final {
        HRESULT result = ::MFStartup(MF_VERSION);
        ~MediaFoundationLifetime() { if (SUCCEEDED(result)) ::MFShutdown(); }
    };
    static const MediaFoundationLifetime lifetime;
    const HRESULT startup_result = lifetime.result;

    if (FAILED(startup_result)) {
        failure.set(fail_code::backend_unavailable,
                    "Media Foundation 初始化失败 hr=0x" + std::to_string(static_cast<unsigned long>(startup_result)));
        return false;
    }
    return true;
}

// MF 的 Source Reader 不认正斜杠路径形式，必须换成反斜杠。
// （实测：给 "D:/x/y.mp3" 会返回 0x80070003 ERROR_PATH_NOT_FOUND。）
[[nodiscard]] std::wstring to_native_path(std::wstring path) {
    for (wchar_t& ch : path) {
        if (ch == L'/') {
            ch = static_cast<wchar_t>(92);
        }
    }
    return path;
}

[[nodiscard]] std::string hresult_hex(HRESULT hr) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

// 打开一个已把输出类型固定为 PCM16 的 Source Reader。
[[nodiscard]] bool open_pcm_reader(const std::filesystem::path& path, ComPtr<IMFSourceReader>& reader,
                                   Failure& failure) {
    if (!ensure_media_foundation(failure)) {
        return false;
    }

    const std::wstring native_path = to_native_path(path.wstring());
    HRESULT hr = ::MFCreateSourceReaderFromURL(native_path.c_str(), nullptr, reader.GetAddressOf());
    if (FAILED(hr) || !reader) {
        // 退一步：显式开字节流再建 reader。路径本身有问题时这条也会失败，
        // 但两者的 HRESULT 不同，能区分"路径不对"和"格式不支持"。
        ComPtr<IMFByteStream> stream;
        const HRESULT file_hr = ::MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                                               MF_FILEFLAGS_NONE, native_path.c_str(),
                                               stream.GetAddressOf());
        if (FAILED(file_hr) || !stream) {
            failure.set(fail_code::file_open_failed,
                        path_to_utf8(path) + " (fromURL " + hresult_hex(hr) + ", MFCreateFile " +
                            hresult_hex(file_hr) + ")");
            return false;
        }
        hr = ::MFCreateSourceReaderFromByteStream(stream.Get(), nullptr, reader.GetAddressOf());
        if (FAILED(hr) || !reader) {
            failure.set(fail_code::unsupported_format,
                        path_to_utf8(path) + " 无法建立 Source Reader (" + hresult_hex(hr) + ")");
            return false;
        }
    }

    // 把输出类型固定成 16 位 PCM。只设 subtype 的话 MF 可能协商出 float32，
    // 那样后面的样本拷贝就会把数据解释错 —— 宁可显式要 16 位。
    ComPtr<IMFMediaType> output_type;
    hr = ::MFCreateMediaType(output_type.GetAddressOf());
    if (FAILED(hr)) {
        failure.set(fail_code::platform_api_failed, "MFCreateMediaType 失败 " + hresult_hex(hr));
        return false;
    }
    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    output_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

    hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
                                     output_type.Get());
    if (FAILED(hr)) {
        failure.set(fail_code::unsupported_format,
                    path_to_utf8(path) + " 无法转换为 PCM (" + hresult_hex(hr) + ")");
        return false;
    }
    return true;
}

[[nodiscard]] bool read_current_format(IMFSourceReader* reader, AudioFormat& format, Failure& failure) {
    ComPtr<IMFMediaType> current;
    HRESULT hr = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                             current.GetAddressOf());
    if (FAILED(hr) || !current) {
        failure.set(fail_code::decode_failed, "GetCurrentMediaType 失败 " + hresult_hex(hr));
        return false;
    }
    UINT32 rate = 0, channels = 0, bits = 0;
    current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    current->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);

    if (rate == 0 || channels == 0) {
        failure.set(fail_code::decode_failed, "MF 协商出的格式不完整");
        return false;
    }
    // 明确失败好过把 float32 当 int16 拷出去
    if (bits != 16) {
        failure.set(fail_code::unsupported_format,
                    "MF 给出 " + std::to_string(bits) + " 位样本，本后端只处理 16 位");
        return false;
    }
    format.sample_rate = rate;
    format.channels = static_cast<std::uint16_t>(channels);
    format.bits_per_sample = static_cast<std::uint16_t>(bits);
    return true;
}

[[nodiscard]] bool read_duration(IMFSourceReader* reader, double& seconds) {
    PROPVARIANT value{};
    ::PropVariantInit(&value);
    bool ok = false;
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                   MF_PD_DURATION, &value))) {
        seconds = static_cast<double>(value.uhVal.QuadPart) / 10000000.0;   // 100ns 单位
        ok = true;
    }
    ::PropVariantClear(&value);
    return ok;
}

}  // namespace

bool mf_probe(const std::filesystem::path& path, AudioInfo& info, Failure& failure) {
    ComApartment apartment;
    if (!apartment.initialize(COINIT_MULTITHREADED, failure)) return false;
    ComPtr<IMFSourceReader> reader;
    if (!open_pcm_reader(path, reader, failure)) {
        return false;
    }
    if (!read_current_format(reader.Get(), info.format, failure)) {
        return false;
    }
    if (!read_duration(reader.Get(), info.duration_seconds)) {
        failure.set(fail_code::decode_failed, path_to_utf8(path) + " 取不到时长");
        return false;
    }
    return true;
}

bool mf_read_chunks(const std::filesystem::path& path, AudioChunkConsumer& consumer,
                     Failure& failure, const std::atomic<bool>* cancelled, double start_seconds) {
    ComApartment apartment;
    if (!apartment.initialize(COINIT_MULTITHREADED, failure)) return false;
    ComPtr<IMFSourceReader> reader;
    if (!open_pcm_reader(path, reader, failure)) return false;
    AudioFormat format;
    if (!read_current_format(reader.Get(), format, failure)) return false;
    const LONGLONG desired_time = static_cast<LONGLONG>(start_seconds * 10000000.0);
    // For the short preloaded boundary, sequential preroll preserves MP3
    // decoder history and exact sample alignment. Timestamp seeking can land
    // on a different compressed frame and change the samples at the join.
    const bool sequential_preroll = start_seconds <= 5.0;
    std::uint64_t discarded_frames = 0;
    const auto desired_frame = static_cast<std::uint64_t>(start_seconds * format.sample_rate + 0.5);
    if (desired_time > 0 && !sequential_preroll) {
        PROPVARIANT position{};
        position.vt = VT_I8;
        position.hVal.QuadPart = desired_time;
        const auto hr = reader->SetCurrentPosition(GUID_NULL, position);
        if (FAILED(hr)) {
            failure.set(fail_code::decode_failed, path_to_utf8(path) + " SetCurrentPosition " + hresult_hex(hr)); return false;
        }
    }
    std::size_t total_frames = 0;
    for (;;) {
        if (cancelled && cancelled->load()) {
            failure.set("audio.cancelled", path_to_utf8(path) + " decoding cancelled");
            return false;
        }
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                        0, nullptr, &flags, &timestamp, sample.GetAddressOf());
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            failure.set(fail_code::decode_failed, path_to_utf8(path) + " ReadSample " + hresult_hex(hr));
            return false;
        }
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            failure.set(fail_code::decode_failed, path_to_utf8(path) + " PCM format changed midstream");
            return false;
        }
        // EOS may accompany a final sample: consume it before leaving the loop.
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            hr = sample->ConvertToContiguousBuffer(buffer.GetAddressOf());
            if (FAILED(hr) || !buffer) {
                failure.set(fail_code::decode_failed, path_to_utf8(path) + " ConvertToContiguousBuffer " + hresult_hex(hr));
                return false;
            }
            BYTE* data = nullptr;
            DWORD capacity = 0, length = 0;
            hr = buffer->Lock(&data, &capacity, &length);
            if (FAILED(hr)) {
                failure.set(fail_code::decode_failed, path_to_utf8(path) + " Lock " + hresult_hex(hr));
                return false;
            }
            const std::size_t frame_bytes = format.channels * sizeof(std::int16_t);
            bool consumed = true;
            if ((length > 0 && data == nullptr) || length % frame_bytes != 0) {
                failure.set(fail_code::decode_failed, path_to_utf8(path) + " incomplete PCM frame bytes=" + std::to_string(length));
                consumed = false;
            } else if (length > 0) {
                const auto frames = length / frame_bytes;
                const auto skip = sequential_preroll
                    ? static_cast<std::size_t>(std::min<std::uint64_t>(frames,
                        desired_frame - std::min(desired_frame, discarded_frames)))
                    : (timestamp >= desired_time ? std::size_t{0} :
                        std::min<std::size_t>(frames, static_cast<std::size_t>(
                            ((desired_time - timestamp) * format.sample_rate + 5000000) / 10000000)));
                discarded_frames += skip;
                if (skip < frames) {
                    consumed = consumer.consume(format, reinterpret_cast<const std::int16_t*>(data) + skip * format.channels,
                                                  frames - skip, failure);
                    total_frames += frames - skip;
                }
            }
            hr = buffer->Unlock();
            if (FAILED(hr)) {
                failure.set(fail_code::decode_failed, path_to_utf8(path) + " Unlock " + hresult_hex(hr));
                return false;
            }
            if (!consumed) return false;
        }
        if (consumer.finished() || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
    }
    if (total_frames == 0) {
        failure.set(fail_code::decode_failed, path_to_utf8(path) + " decoded zero frames");
        return false;
    }
    return true;
}

}  // namespace musicboard::detail
