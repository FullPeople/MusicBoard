// 曲库扫描工具：阶段 1 的验收证据
//
// 它回答一个具体问题：把 3.7 GB / 491 个文件的曲库过一遍，需要多久、占多少内存？
//
// 这正是"probe 与 decode 必须分开"这个设计的判据。Godot 原型在扫描阶段就把
// 每个文件整个读进内存，那样做的常驻内存是 GB 级；这个工具只用 probe。
//
// 用法: scan_library.exe <曲库根目录>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "audio/decoder.h"
#include "core/path_utf8.h"

#pragma comment(lib, "psapi.lib")

namespace {

std::size_t current_working_set_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters))) {
        return counters.WorkingSetSize;
    }
    return 0;
}

std::string to_mb(std::size_t bytes) {
    return std::to_string(bytes / (1024 * 1024)) + " MB";
}

std::string lowercase_extension(const std::filesystem::path& path) {
    std::string ext = musicboard::path_to_utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

}  // namespace

// 必须用 wmain：MSVC 的 char** argv 走活动代码页(936)，中文路径会被打乱
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("用法: scan_library.exe <曲库根目录>\n");
        return 2;
    }

    const std::filesystem::path root(argv[1]);
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        std::printf("目录不存在: %s\n", musicboard::path_to_utf8(root).c_str());
        return 2;
    }

    std::printf("扫描曲库: %s\n", musicboard::path_to_utf8(root).c_str());
    const std::size_t memory_before = current_working_set_bytes();

    // 收集文件清单（这一步本身也要计入耗时 —— 它是扫描成本的一部分）
    std::vector<std::filesystem::path> files;
    const auto collect_start = std::chrono::steady_clock::now();
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string ext = lowercase_extension(it->path());
        if (ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".m4a" ||
            ext == ".aac" || ext == ".wma") {
            files.push_back(it->path());
        }
    }
    const auto collect_end = std::chrono::steady_clock::now();

    std::printf("发现 %zu 个音频文件\n", files.size());
    if (files.empty()) {
        return 1;
    }

    // 逐个 probe（只读格式与时长，不解码）
    std::map<std::string, int> by_container;
    std::vector<std::string> failures;
    double total_duration = 0.0;
    const auto probe_start = std::chrono::steady_clock::now();

    for (const auto& path : files) {
        musicboard::AudioInfo info;
        musicboard::Failure failure;
        if (musicboard::probe_audio_file(path, info, failure)) {
            ++by_container[musicboard::container_name(info.container)];
            total_duration += info.duration_seconds;
        } else if (failures.size() < 10) {   // 只留前几条，够定位就行
            failures.push_back(musicboard::path_to_utf8(path) + " -> " + failure.to_string());
        }
    }
    const auto probe_end = std::chrono::steady_clock::now();
    const std::size_t memory_after = current_working_set_bytes();

    const auto collect_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(collect_end - collect_start).count();
    const auto probe_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(probe_end - probe_start).count();

    std::printf("\n---- 结果 ----\n");
    std::printf("列目录耗时      : %lld ms\n", static_cast<long long>(collect_ms));
    std::printf("probe 耗时      : %lld ms  (平均 %.2f ms/文件)\n", static_cast<long long>(probe_ms),
                files.empty() ? 0.0 : static_cast<double>(probe_ms) / static_cast<double>(files.size()));
    std::printf("总时长          : %.1f 小时\n", total_duration / 3600.0);
    std::printf("内存占用增量    : %s  (前 %s / 后 %s)\n",
                to_mb(memory_after > memory_before ? memory_after - memory_before : 0).c_str(),
                to_mb(memory_before).c_str(), to_mb(memory_after).c_str());
    std::printf("按容器统计      :\n");
    for (const auto& [name, count] : by_container) {
        std::printf("    %-8s %d\n", name.c_str(), count);
    }
    if (!failures.empty()) {
        std::printf("失败样例（最多列 10 条）:\n");
        for (const auto& line : failures) {
            std::printf("    %s\n", line.c_str());
        }
    }

    const int total_probed = [&] {
        int sum = 0;
        for (const auto& [name, count] : by_container) sum += count;
        return sum;
    }();
    std::printf("成功 %d / %zu\n", total_probed, files.size());
    return total_probed == static_cast<int>(files.size()) ? 0 : 1;
}
