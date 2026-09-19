// 假下载器：给 process_runner / import_job 的测试当子进程用。
//
// 为什么需要它：进程管理那一层（CreateProcess + 管道 + Job Object + 取消）
// 是"进度永远不动"和"取消是假的"这两个原版问题的所在地，最该被测到 ——
// 但正对着真的 yt-dlp 测就等于对着网络测，那样的测试没人愿意跑，
// 也没人敢信（站点一变就红）。
//
// 两种调用方式：
//
//   1. **直接模式**（process_runner 的测试用）：argv[1] = 模式，argv[2] = 目录。
//   2. **yt-dlp 模式**（import_job 的测试用）：设了环境变量 MUSICBOARD_FAKE_MODE，
//      参数表则是 import_job 拼出来的**真实 yt-dlp 参数**。它从 `-o` 模板里
//      解析出该写到哪个目录 —— 于是"我们的参数拼得对不对、子进程能不能用"
//      也一起被验证了。引号或路径拼错的话，这些用例会直接红。
//
// 两种模式都按 argv[1] 指定的模式打印 yt-dlp 格式的输出、往目录里写文件，
// 然后按模式退出或睡下去。测试因此可以精确要求"进度分 10 次报出来"
// "输出到一半不许退"这种时序。
//
// 每个模式对应一个真实会发生的场景：
//   success    正常下完并产出一个可解码的音频文件
//   progress   分多次报进度、每次间隔一小会儿（验证进度是真的在动）
//   fail       打一行 ERROR 然后非零退出（验证错误信息被提取出来）
//   refusal    403/404 一类的错误（验证建议机制）
//   silent     什么都不打然后睡很久（验证卡死判定）
//   hang       一直报进度但永不退出（验证取消能真的杀掉它）
//   junk       产出一个改名成 .mp3 的文本文件（验证入库事务会挡住非音频）
//   empty      报 100% 然后成功退出，但一个文件都不产出（验证"不猜文件名"）
//   two_files  产出两个文件（验证"数量不对就不猜"）
//   spam       疯狂输出（验证累积上限）
//   echo_args  把参数原样以 UTF-8 打出来（验证引号与 UTF-8 转换）
//
// 用 wmain：它要接收带中文的暂存目录路径。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void emit(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void sleep_milliseconds(int milliseconds) {
    ::Sleep(static_cast<DWORD>(milliseconds));
}

// 宽字符 → UTF-8。子进程的内部一律用这个，别让 CRT 按活动代码页转。
std::string to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed,
                          nullptr, nullptr);
    return out;
}

// 一个合法的 8 kHz 单声道 16 bit WAV。与 tests/test_files.h 里的一致 ——
// 这里不能 include 它，那是测试自己的头文件。
std::vector<std::uint8_t> make_wav_bytes(std::uint32_t duration_ms) {
    constexpr std::uint32_t sample_rate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    constexpr std::uint16_t block_align = channels * bits / 8;
    const std::uint32_t frame_count = sample_rate * duration_ms / 1000;
    const std::uint32_t data_size = frame_count * block_align;
    const std::uint32_t byte_rate = sample_rate * block_align;

    std::vector<std::uint8_t> bytes;
    const auto push32 = [&bytes](std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    };
    const auto push16 = [&bytes](std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    };
    const auto push_tag = [&bytes](const char* tag) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(tag[i]));
        }
    };

    push_tag("RIFF");
    push32(36 + data_size);
    push_tag("WAVE");
    push_tag("fmt ");
    push32(16);
    push16(1);   // PCM
    push16(channels);
    push32(sample_rate);
    push32(byte_rate);
    push16(block_align);
    push16(bits);
    push_tag("data");
    push32(data_size);
    for (std::uint32_t i = 0; i < frame_count; ++i) {
        push16(((i / 4) % 2 == 0) ? 8000 : 1000);
    }
    return bytes;
}

bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
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

bool write_wav(const std::filesystem::path& path) {
    return write_bytes(path, make_wav_bytes(200));
}

// 从真实的 yt-dlp 参数表里解析出 `-o <模板>` 的目录部分。
// 模板形如 C:\...\staging\job-xxx\%(title)s.%(ext)s，取最后一个分隔符之前的部分。
std::filesystem::path directory_from_output_template(const std::wstring& template_text) {
    const std::size_t cut = template_text.find_last_of(L"\\/");
    if (cut == std::wstring::npos) {
        return {};
    }
    return std::filesystem::path(template_text.substr(0, cut));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 不缓冲：测试要能在子进程还在跑的时候读到已经打出来的行
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // yt-dlp 模式：模式来自环境变量，目录来自 -o 模板
    wchar_t mode_buffer[64]{};
    const DWORD mode_length = ::GetEnvironmentVariableW(L"MUSICBOARD_FAKE_MODE", mode_buffer,
                                                        static_cast<DWORD>(std::size(mode_buffer)));
    const bool yt_dlp_mode = mode_length > 0 && mode_length < std::size(mode_buffer);

    std::wstring mode;
    std::filesystem::path directory;

    if (yt_dlp_mode) {
        mode = mode_buffer;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::wstring(argv[i]) == L"-o") {
                directory = directory_from_output_template(argv[i + 1]);
                break;
            }
        }
        if (directory.empty()) {
            std::fputs("fake_child: yt-dlp 模式的参数里没有可用的 -o 模板\n", stderr);
            return 2;
        }
    } else {
        if (argc < 3) {
            std::fputs("fake_child: 需要 <模式> <目录>\n", stderr);
            return 2;
        }
        mode = argv[1];
        directory = argv[2];
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    if (mode == L"success") {
        emit("[youtube] abcdef: Downloading webpage");
        emit("[download] Destination: " + to_utf8(directory.wstring()) + "/raw.webm");
        emit("[download]  50.0% of 1.00MiB at 1.00MiB/s ETA 00:01");
        emit("[download] 100% of 1.00MiB in 00:01");
        emit("[ExtractAudio] Destination: song.mp3");
        return write_wav(directory / L"下载来的曲子.wav") ? 0 : 3;
    }

    if (mode == L"progress") {
        // 每次只前进一点，并且真的花时间 —— 测试因此能在"还没下完"的时候
        // 就观察到中间进度。这正是原版做不到的那件事。
        for (int percent = 10; percent <= 100; percent += 10) {
            char line[128]{};
            std::snprintf(line, sizeof(line), "[download] %5.1f%% of 1.00MiB at 1.00MiB/s",
                          static_cast<double>(percent));
            emit(line);
            sleep_milliseconds(60);
        }
        return write_wav(directory / L"分步下载.wav") ? 0 : 3;
    }

    if (mode == L"fail") {
        emit("[youtube] abcdef: Downloading webpage");
        emit("WARNING: something odd happened");
        emit("ERROR: Unable to download webpage: HTTP Error 404: Not Found");
        return 1;
    }

    if (mode == L"refusal") {
        emit("ERROR: unable to download video data: HTTP Error 403: Forbidden");
        return 1;
    }

    if (mode == L"silent") {
        // 一个字都不打，然后睡很久 —— 触发卡死判定
        sleep_milliseconds(30000);
        return 0;
    }

    if (mode == L"hang") {
        // 一直报进度但永不结束 —— 验证取消真的杀得掉它
        for (int i = 0; i < 6000; ++i) {
            emit("[download]   1.0% of 1.00MiB at 1.00MiB/s");
            sleep_milliseconds(20);
        }
        return 0;
    }

    if (mode == L"junk") {
        emit("[download] 100% of 1.00MiB in 00:01");
        const std::string text = "这不是音频";
        return write_bytes(directory / L"伪装.mp3",
                           std::vector<std::uint8_t>(text.begin(), text.end()))
                   ? 0
                   : 3;
    }

    if (mode == L"empty") {
        emit("[download] 100% of 0.00MiB in 00:00");
        emit("[ExtractAudio] Destination: never-created.mp3");
        return 0;   // 报成功，但一个文件都不产出
    }

    if (mode == L"two_files") {
        emit("[download] 100% of 1.00MiB in 00:01");
        const bool first = write_wav(directory / L"第一个.wav");
        const bool second = write_wav(directory / L"第二个.wav");
        return (first && second) ? 0 : 3;
    }

    if (mode == L"spam") {
        // 疯狂输出，用来验证累积上限（测试会把上限调得很小）
        for (int i = 0; i < 4000; ++i) {
            emit("[download]  12.3% of 1.00MiB at 1.00MiB/s padding padding padding padding");
        }
        return 0;
    }

    if (mode == L"echo_args") {
        // 把收到的参数原样以 UTF-8 打出来 —— 验证引号与 UTF-8 转换
        for (int i = 2; i < argc; ++i) {
            emit("ARG: " + to_utf8(argv[i]));
        }
        return 0;
    }

    std::fputs("fake_child: 未知模式\n", stderr);
    return 2;
}
