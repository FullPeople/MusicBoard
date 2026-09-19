#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#include "core/failure.h"

namespace musicboard {

// ============================================================================
// 日志：JSONL 追加写，落在 %LOCALAPPDATA%\MusicBoard\logs
//
// 由构造函数注入给需要的模块（见 audio 层），而不是做成全局单例 ——
// 这样测试里可以塞一个临时目录的 logger，不需要清理全局状态。
//
// 所有路径经 path_to_utf8 输出，保证中文曲名在日志里是可读的 UTF-8，
// 而不是一串问号。
// ============================================================================

enum class LogLevel { info, warning, error };

class Logger final {
public:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 默认目录：%LOCALAPPDATA%\MusicBoard\logs
    [[nodiscard]] bool initialize(Failure& failure);

    // 指定目录（测试用）
    [[nodiscard]] bool initialize_in_directory(const std::filesystem::path& directory, Failure& failure);

    void write(LogLevel level, std::string_view event, std::string_view context);

    [[nodiscard]] bool is_open() const noexcept { return stream_.is_open(); }
    [[nodiscard]] const std::filesystem::path& log_path() const noexcept { return log_path_; }

private:
    static const char* level_name(LogLevel level) noexcept;

    std::mutex mutex_;
    std::ofstream stream_;
    std::filesystem::path log_path_;
};

}  // namespace musicboard
