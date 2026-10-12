#pragma once

// ============================================================================
// 测试用的临时目录与最小音频素材
//
// 为什么要生成 WAV 而不是"找一首真的"：
// library_ops / import_transaction 这两个测试要跑几十次真实文件操作，
// 如果每次都对着 GB 级的曲库复制一遍，测试会慢到没人愿意跑。
// 自己写一个 200 毫秒的合法 WAV，既快又不需要外部素材存在 ——
// 而"真实大曲库"那部分由 library_scanner_tests 单独对着 491 个文件跑。
//
// 全程只走宽字符 API。临时目录里可能带中文用户名（C:\Users\张三\AppData\...），
// 用窄字符路径在这里同样是那个"必然失败"的坑。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace musicboard::test {

// 与当前测试可执行放在一起的假下载器（tests/fake_child.cpp）。
// 靠"自己的 exe 在哪"来找，不靠环境变量或当前目录 —— 多配置生成器、
// 从别的目录调 ctest、IDE 里单跑，三种情况都不会指错。
[[nodiscard]] inline std::filesystem::path fake_child_path() {
    wchar_t buffer[32768]{};
    const DWORD written =
        ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path() / L"fake_child.exe";
}

// 反复调用 step() 直到 done() 为真。每次 step 之间真的等一小会儿。
//
// 注意这里等的是**真实时间**：进程的存活、管道里的字节、退出码，
// 都是操作系统按真实时间推进的，注入假时钟只会让测试自欺。
// 所以进程相关的测试用很短的超时，并且允许它真的花上一两秒。
[[nodiscard]] inline bool wait_until(const std::function<bool()>& done,
                                    const std::function<void()>& step, int timeout_ms = 20000,
                                    int step_delay_ms = 5) {
    const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    for (;;) {
        step();
        if (done()) {
            return true;
        }
        if (::GetTickCount64() >= deadline) {
            return false;
        }
        ::Sleep(static_cast<DWORD>(step_delay_ms));
    }
}

// %LOCALAPPDATA%\MusicBoard\tests\t<pid>_<序号>。构造时清空，析构时删掉。
class TempDir final {
public:
    TempDir() {
        wchar_t buffer[32768]{};
        const DWORD written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                       static_cast<DWORD>(std::size(buffer)));
        std::filesystem::path base;
        if (written == 0 || written >= std::size(buffer)) {
            base = std::filesystem::temp_directory_path();
        } else {
            base = std::filesystem::path(buffer) / L"MusicBoard" / L"tests";
        }

        static std::atomic<unsigned> counter{0};
        path_ = base / (L"t" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
                        std::to_wstring(counter.fetch_add(1)));

        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// 最小文件读写（宽字符，绕开 CRT 的窄字符路径）
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool write_bytes(const std::filesystem::path& path,
                                      const std::vector<std::uint8_t>& bytes) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = bytes.empty()
                        ? TRUE
                        : ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                      nullptr);
    ::CloseHandle(file);
    return ok != FALSE && written == bytes.size();
}

[[nodiscard]] inline bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    return write_bytes(path, bytes);
}

// 读回文本，用于断言"没有覆盖原有文件"。读不到就返回空串。
[[nodiscard]] inline std::string read_text(const std::filesystem::path& path) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string out;
    char buffer[4096];
    DWORD read = 0;
    while (::ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read > 0) {
        out.append(buffer, read);
    }
    ::CloseHandle(file);
    return out;
}

// ---------------------------------------------------------------------------
// 一个合法但极小的 WAV：8 kHz 单声道 16 bit。
// 这些参数是刻意的 —— Media Foundation 对 8 kHz PCM 一律接受，
// 而 44.1 kHz 在某些"独占模式"配置下反而更挑。
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<std::uint8_t> make_wav_bytes(std::uint32_t duration_ms) {
    constexpr std::uint32_t sample_rate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    constexpr std::uint16_t block_align = channels * bits / 8;

    const std::uint32_t frame_count = sample_rate * duration_ms / 1000;
    const std::uint32_t data_size = frame_count * block_align;
    const std::uint32_t riff_size = 36 + data_size;
    const std::uint32_t byte_rate = sample_rate * block_align;

    const auto push32 = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    };
    const auto push16 = [](std::vector<std::uint8_t>& out, std::uint16_t value) {
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    };
    const auto push_tag = [](std::vector<std::uint8_t>& out, const char* tag) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(tag[i]));
        }
    };

    std::vector<std::uint8_t> bytes;
    bytes.reserve(44 + data_size);
    push_tag(bytes, "RIFF");
    push32(bytes, riff_size);
    push_tag(bytes, "WAVE");
    push_tag(bytes, "fmt ");
    push32(bytes, 16);
    push16(bytes, 1);   // PCM
    push16(bytes, channels);
    push32(bytes, sample_rate);
    push32(bytes, byte_rate);
    push16(bytes, block_align);
    push16(bytes, bits);
    push_tag(bytes, "data");
    push32(bytes, data_size);

    // 一段简单方波，别指望好听 —— 只要不是全零（有的解码器直接把全零流当损坏）
    for (std::uint32_t i = 0; i < frame_count; ++i) {
        const std::int16_t value = ((i / 4) % 2 == 0) ? 8000 : -8000;
        push16(bytes, static_cast<std::uint16_t>(value));
    }
    return bytes;
}

// 写一个能 probe / decode 通过的音频文件
[[nodiscard]] inline bool write_test_wav(const std::filesystem::path& path,
                                        std::uint32_t duration_ms = 200) {
    return write_bytes(path, make_wav_bytes(duration_ms));
}

}  // namespace musicboard::test
