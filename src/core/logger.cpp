#include "core/logger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include "core/path_utf8.h"

namespace musicboard {
namespace {

constexpr char kQuote = static_cast<char>(34);   // "
constexpr char kBackslash = static_cast<char>(92);  // \

// JSON 字符串转义。日志里会出现中文曲名与 Windows 路径（反斜杠遍地），
// 不转义的话 JSONL 直接不可解析。
void append_json_escaped(std::string& out, std::string_view text) {
    out.push_back(kQuote);
    for (const char ch : text) {
        switch (ch) {
            case kQuote:      out.push_back(kBackslash); out.push_back(kQuote); break;
            case kBackslash:  out.push_back(kBackslash); out.push_back(kBackslash); break;
            case '\n':        out.push_back(kBackslash); out.push_back('n'); break;
            case '\r':        out.push_back(kBackslash); out.push_back('r'); break;
            case '\t':        out.push_back(kBackslash); out.push_back('t'); break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out.push_back(ch);   // UTF-8 多字节的高位字节原样保留
                }
        }
    }
    out.push_back(kQuote);
}

std::filesystem::path default_log_directory() {
    // 用宽字符 API 取，避免 %LOCALAPPDATA% 里含非 ASCII 用户名时的编码问题
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                  static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return std::filesystem::path(L"logs");   // 取不到就落到当前目录，调用方可从 log_path 看出
    }
    return std::filesystem::path(buffer) / L"MusicBoard" / L"logs";
}

}  // namespace

Logger::~Logger() = default;

const char* Logger::level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::info:    return "info";
        case LogLevel::warning: return "warning";
        case LogLevel::error:   return "error";
    }
    return "unknown";
}

bool Logger::initialize(Failure& failure) {
    return initialize_in_directory(default_log_directory(), failure);
}

bool Logger::initialize_in_directory(const std::filesystem::path& directory, Failure& failure) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        failure.set(fail_code::file_open_failed,
                    "无法创建日志目录 " + path_to_utf8(directory) + ": " + ec.message());
        return false;
    }

    // 每次启动一个新文件，避免单文件无限增长；文件名带本地时间
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &t);
    std::ostringstream name;
    name << "musicboard-" << std::put_time(&local, "%Y%m%d-%H%M%S") << ".jsonl";

    log_path_ = directory / name.str();
    stream_.open(log_path_, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream_.is_open()) {
        failure.set(fail_code::file_open_failed, "无法打开日志文件 " + path_to_utf8(log_path_));
        return false;
    }
    return true;
}

void Logger::write(LogLevel level, std::string_view event, std::string_view context) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_.is_open()) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &t);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count() % 1000;

    std::string escaped;   // 复用一块缓冲，避免每行多次分配
    std::ostringstream line;
    line << "{"
         << "\"time\":\"" << std::put_time(&local, "%Y-%m-%dT%H:%M:%S")
         << "." << std::setw(3) << std::setfill('0') << ms << "\","
         << "\"level\":\"" << level_name(level) << "\","
         << "\"event\":";
    append_json_escaped(escaped, event);
    line << escaped << ",\"context\":";
    escaped.clear();
    append_json_escaped(escaped, context);
    line << escaped << "}\n";

    stream_ << line.str();
    stream_.flush();   // 崩溃时最后几行还在，比吞吐重要
}

}  // namespace musicboard
