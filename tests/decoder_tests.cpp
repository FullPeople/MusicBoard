// 解码器测试：对着真实曲库跑
//
// 曲库位置由环境变量 MUSICBOARD_TEST_LIBRARY 给出。没有就 SKIP 而不是 FAIL ——
// 测试的成败不该取决于某台机器上有没有那份素材。
//
// 这个测试冲着的核心问题：
//   1. 中文/全角符号路径能不能走通（这是本项目的命门）
//   2. 各容器是否都由正确的后端接住（ogg 不能落到 Media Foundation 上）
//   3. probe 与 decode 的时长是否一致（两条路径读的是同一个文件）
//   4. 失败的时候有没有给出可定位的错误码，而不是静默返回

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <string>
#include <vector>

#include "audio/decoder.h"
#include "core/file_bytes.h"
#include "core/path_utf8.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

std::filesystem::path library_root() {
    // 必须用宽字符 API 读环境变量。
    // 不能用 getenv：它在 Windows 上按活动代码页(936)返回，
    // 含中文的曲库路径会被打乱 —— 和 char** argv 是同一个坑。
    wchar_t buffer[32768]{};   // 环境变量长度的实际上限
    const DWORD written = ::GetEnvironmentVariableW(L"MUSICBOARD_TEST_LIBRARY", buffer,
                                                   static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    const std::filesystem::path path(buffer);
    return std::filesystem::is_directory(path) ? path : std::filesystem::path{};
}

// 收集找到的音频文件（最多 limit 个），按扩展名分组
struct Sample {
    std::vector<std::filesystem::path> mp3;
    std::vector<std::filesystem::path> ogg;
    std::vector<std::filesystem::path> wav;
    std::vector<std::filesystem::path> all;

    [[nodiscard]] std::size_t size() const { return all.size(); }
};

Sample collect(const std::filesystem::path& root, std::size_t limit_per_kind) {
    Sample sample;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        return sample;
    }
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string ext = path_to_utf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::vector<std::filesystem::path>* bucket = nullptr;
        if (ext == ".mp3") bucket = &sample.mp3;
        else if (ext == ".ogg") bucket = &sample.ogg;
        else if (ext == ".wav") bucket = &sample.wav;
        if (bucket == nullptr || bucket->size() >= limit_per_kind) {
            continue;
        }
        bucket->push_back(entry.path());
        sample.all.push_back(entry.path());
        if (sample.mp3.size() >= limit_per_kind && sample.ogg.size() >= limit_per_kind &&
            sample.wav.size() >= limit_per_kind) {
            break;
        }
    }
    return sample;
}

// 挑一个含非 ASCII 字符的路径 —— 这才是真正要验的东西
std::filesystem::path pick_non_ascii(const std::vector<std::filesystem::path>& paths) {
    for (const auto& p : paths) {
        const std::string utf8 = path_to_utf8(p);
        for (unsigned char ch : utf8) {
            if (ch >= 0x80) {
                return p;
            }
        }
    }
    return {};
}

void test_probe_over_library(Registry& r, const Sample& sample) {
    if (sample.size() == 0) {
        r.skip("曲库不存在，跳过全部解码用例");
        return;
    }

    int probed_ok = 0;
    int probed_fail = 0;
    std::string first_failure;

    for (const auto& path : sample.all) {
        AudioInfo info;
        Failure failure;
        if (probe_audio_file(path, info, failure)) {
            ++probed_ok;
            // 时长必须是正数，否则界面上的进度条无处可依
            if (!(info.duration_seconds > 0.0)) {
                r.check_eq(false, "probe: 时长应为正数", path_to_utf8(path));
                return;
            }
            if (info.format.sample_rate == 0 || info.format.channels == 0) {
                r.check_eq(false, "probe: 采样率与声道数应有效", path_to_utf8(path));
                return;
            }
        } else {
            ++probed_fail;
            if (first_failure.empty()) {
                first_failure = path_to_utf8(path) + " -> " + failure.to_string();
            }
        }
    }

    r.check_eq(probed_fail == 0, "probe: 曲库里每个样例都能探出格式与时长",
               "成功 " + std::to_string(probed_ok) + " 失败 " + std::to_string(probed_fail) +
                   " 首个失败: " + first_failure);
}

void test_backend_routing(Registry& r, const Sample& sample) {
    // ogg 必须由 stb_vorbis 接住。如果它落到 Media Foundation，
    // 在这台机器上会得到 MF_E_BYTESOURCE_UNKNOWN_TYPE。
    if (!sample.ogg.empty()) {
        DecodedAudio audio;
        Failure failure;
        const bool ok = decode_audio_file(sample.ogg.front(), audio, failure);
        r.check_eq(ok, "ogg 能解码", path_to_utf8(sample.ogg.front()) + " -> " + failure.to_string());
        if (ok) {
            r.check_eq(std::string(audio.backend) == "stb_vorbis", "ogg 由 stb_vorbis 后端处理",
                       std::string("实际后端 ") + audio.backend);
            r.check(audio.format.sample_rate > 0 && audio.format.channels > 0, "ogg 格式有效");
            r.check(!audio.samples.empty(), "ogg 解出了样本");
        }
    } else {
        r.skip("ogg 样例缺失");
    }

    if (!sample.mp3.empty()) {
        DecodedAudio audio;
        Failure failure;
        const bool ok = decode_audio_file(sample.mp3.front(), audio, failure);
        r.check_eq(ok, "mp3 能解码", path_to_utf8(sample.mp3.front()) + " -> " + failure.to_string());
        if (ok) {
            r.check_eq(std::string(audio.backend) == "media_foundation", "mp3 由 Media Foundation 后端处理",
                       std::string("实际后端 ") + audio.backend);
            r.check(!audio.samples.empty(), "mp3 解出了样本");
        }
    } else {
        r.skip("mp3 样例缺失");
    }

    if (!sample.wav.empty()) {
        DecodedAudio audio;
        Failure failure;
        const bool ok = decode_audio_file(sample.wav.front(), audio, failure);
        r.check_eq(ok, "wav 能解码", path_to_utf8(sample.wav.front()) + " -> " + failure.to_string());
        if (ok) {
            r.check(!audio.samples.empty(), "wav 解出了样本");
        }
    } else {
        r.skip("wav 样例缺失");
    }
}

void test_non_ascii_path(Registry& r, const Sample& sample) {
    const std::filesystem::path path = pick_non_ascii(sample.all);
    if (path.empty()) {
        r.skip("曲库里没有含非 ASCII 字符的路径");
        return;
    }
    r.check(true, "找到含非 ASCII 的路径: " + path_to_utf8(path));

    AudioInfo info;
    Failure failure;
    r.check_eq(probe_audio_file(path, info, failure), "非 ASCII 路径可以探测",
               path_to_utf8(path) + " -> " + failure.to_string());

    DecodedAudio audio;
    Failure decode_failure;
    r.check_eq(decode_audio_file(path, audio, decode_failure), "非 ASCII 路径可以解码",
               path_to_utf8(path) + " -> " + decode_failure.to_string());
}

void test_probe_matches_decode(Registry& r, const Sample& sample) {
    // 两条路径读的是同一个文件，时长不该有明显出入。
    // 容差 0.5 秒：容器头里的时长与逐帧累计的时长允许有尾差。
    for (const auto& path : sample.all) {
        AudioInfo info;
        Failure probe_failure;
        if (!probe_audio_file(path, info, probe_failure)) {
            continue;
        }
        DecodedAudio audio;
        Failure decode_failure;
        if (!decode_audio_file(path, audio, decode_failure)) {
            continue;
        }
        const double diff = info.duration_seconds - audio.duration_seconds;
        if (diff > 0.5 || diff < -0.5) {
            r.check_eq(false, "probe 与 decode 的时长应当一致",
                       path_to_utf8(path) + " probe=" + std::to_string(info.duration_seconds) +
                           " decode=" + std::to_string(audio.duration_seconds));
            return;
        }
        if (audio.format.sample_rate != info.format.sample_rate ||
            audio.format.channels != info.format.channels) {
            r.check_eq(false, "probe 与 decode 的格式应当一致", path_to_utf8(path));
            return;
        }
    }
    r.check(true, "probe 与 decode 在所有样例上格式一致、时长差在 0.5 秒内");
}

void test_failures_are_explicit(Registry& r) {
    // 失败必须能定位。这条用例冲的正是"静默失败"。
    const std::filesystem::path missing = library_root().empty()
                                              ? std::filesystem::path(L"不存在的文件.mp3")
                                              : library_root() / L"绝对不存在的文件.mp3";
    AudioInfo info;
    Failure failure;
    r.check(!probe_audio_file(missing, info, failure), "不存在的文件返回失败");
    r.check(!failure.code.empty(), "失败带有错误码");
    r.check(!failure.context.empty(), "失败带有可读上下文");
    r.check_eq(failure.code == fail_code::file_not_found, "不存在的文件给出 file_not_found 错误码",
               failure.to_string());
}

void test_unknown_content(Registry& r) {
    // 内容不是音频时，必须明确说"格式不支持"，而不是当成解码失败。
    std::filesystem::path temp = std::filesystem::temp_directory_path() / L"musicboard_not_audio.bin";
    {
        const std::string text = "这不是音频，只是一段普通文本，用来验证嗅探会明确拒绝。";
        std::ofstream out(temp, std::ios::binary);
        out << text;
    }
    AudioInfo info;
    Failure failure;
    r.check(!probe_audio_file(temp, info, failure), "非音频内容被拒绝");
    r.check_eq(failure.code == fail_code::unsupported_format,
               "非音频内容给出 unsupported_format 而不是 decode_failed", failure.to_string());
    std::error_code ec;
    std::filesystem::remove(temp, ec);
}

}  // namespace

int main() {
    Registry r;
    const std::filesystem::path root = library_root();
    if (root.empty()) {
        r.skip("MUSICBOARD_TEST_LIBRARY 未设置或目录不存在");
    } else {
        r.check(true, "曲库根目录: " + path_to_utf8(root));
    }

    const Sample sample = root.empty() ? Sample{} : collect(root, 6);

    test_failures_are_explicit(r);
    test_unknown_content(r);
    test_probe_over_library(r, sample);
    test_backend_routing(r, sample);
    test_non_ascii_path(r, sample);
    test_probe_matches_decode(r, sample);

    return r.finish("decoder_tests");
}
