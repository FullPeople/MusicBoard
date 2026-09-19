#include "library/staging.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>
#include <system_error>

#include "core/path_utf8.h"

namespace musicboard {
namespace {

[[nodiscard]] std::filesystem::path local_app_data() {
    wchar_t buffer[32768]{};
    const DWORD written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                   static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer);
}

// "20260916-143012"。目录名，不是给人读的时间显示，所以固定格式就好。
[[nodiscard]] std::string timestamp_for_name() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (::localtime_s(&local, &now) != 0) {
        return "00000000-000000";
    }
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    return buffer;
}

[[nodiscard]] bool is_hidden_name(const std::filesystem::path& path) {
    const std::string name = path_to_utf8(path.filename());
    return name.empty() || name.front() == '.';
}

}  // namespace

std::filesystem::path default_staging_root() {
    const std::filesystem::path base = local_app_data();
    if (base.empty()) {
        return {};
    }
    return base / L"MusicBoard" / L"staging";
}

bool create_staging_job_dir(const std::filesystem::path& staging_root,
                            std::filesystem::path& out, Failure& failure) {
    out.clear();
    if (staging_root.empty()) {
        failure.set(fail_code::invalid_argument, "暂存区根目录为空");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(staging_root, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "无法创建暂存区目录 " + path_to_utf8(staging_root) + ": " + ec.message());
        return false;
    }

    static std::atomic<unsigned> counter{0};
    const std::string stamp = timestamp_for_name();
    const auto process_id = static_cast<unsigned long>(::GetCurrentProcessId());

    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::string name =
            "job-" + stamp + "-" + std::to_string(process_id) + "-" +
            std::to_string(counter.fetch_add(1) + static_cast<unsigned>(attempt));
        const std::filesystem::path candidate = staging_root / std::filesystem::path(
            std::wstring(name.begin(), name.end()));
        std::error_code exists_ec;
        if (std::filesystem::exists(candidate, exists_ec)) {
            continue;
        }
        if (std::filesystem::create_directory(candidate, ec) && !ec) {
            out = candidate;
            return true;
        }
        // 撞名或权限问题：换一个名字再试。连续失败到上限才报错。
    }

    failure.set(fail_code::file_open_failed,
                "在 " + path_to_utf8(staging_root) + " 下建不出唯一的新任务目录（试了 1000 次）");
    return false;
}

bool list_staged_files(const std::filesystem::path& job_dir,
                       std::vector<std::filesystem::path>& out, Failure& failure) {
    out.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(job_dir, ec)) {
        failure.set(fail_code::file_not_found, "暂存任务目录不存在: " + path_to_utf8(job_dir));
        return false;
    }

    std::filesystem::directory_iterator it(job_dir, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "无法读取暂存目录 " + path_to_utf8(job_dir) + ": " + ec.message());
        return false;
    }
    for (const auto& entry : it) {
        if (is_hidden_name(entry.path())) {
            continue;
        }
        if (entry.is_regular_file(ec)) {
            out.push_back(entry.path());
        }
    }
    // 排序让结果确定 —— "只有一个文件"之外的情况也要可复现
    std::sort(out.begin(), out.end());
    return true;
}

bool remove_staging_job_dir(const std::filesystem::path& job_dir, Failure& failure) {
    std::error_code ec;
    if (!std::filesystem::exists(job_dir, ec)) {
        return true;   // 已经没了就是想要的终态
    }
    std::filesystem::remove_all(job_dir, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "清理暂存目录失败 " + path_to_utf8(job_dir) + ": " + ec.message());
        return false;
    }
    return true;
}

bool prune_staging(const std::filesystem::path& staging_root, std::chrono::hours keep,
                   std::size_t& removed, Failure& failure) {
    removed = 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(staging_root, ec)) {
        return true;   // 还没有暂存区，没什么可清
    }

    const auto now = std::filesystem::file_time_type::clock::now();
    const auto deadline = std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
        std::chrono::hours(keep));

    std::string first_failure;
    std::filesystem::directory_iterator it(staging_root, ec);
    if (ec) {
        failure.set(fail_code::file_open_failed,
                    "无法读取暂存区 " + path_to_utf8(staging_root) + ": " + ec.message());
        return false;
    }
    for (const auto& entry : it) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const auto name = path_to_utf8(entry.path().filename());
        if (!name.starts_with("job-")) {
            continue;   // 只清我们自己建的
        }
        const auto written = std::filesystem::last_write_time(entry.path(), ec);
        if (ec) {
            if (first_failure.empty()) {
                first_failure = "读不到修改时间: " + path_to_utf8(entry.path());
            }
            continue;
        }
        if (now - written <= deadline) {
            continue;
        }
        std::error_code remove_ec;
        std::filesystem::remove_all(entry.path(), remove_ec);
        if (remove_ec) {
            if (first_failure.empty()) {
                first_failure =
                    path_to_utf8(entry.path()) + ": " + remove_ec.message();
            }
            continue;
        }
        ++removed;
    }

    if (!first_failure.empty()) {
        // 一个删不掉不该让其余的都留着，但也不能什么都不说
        failure.set(fail_code::file_open_failed,
                    "部分旧的导入暂存目录没能清理（" + first_failure + "）");
        return false;
    }
    return true;
}

}  // namespace musicboard
