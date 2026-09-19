#include "library/library_scanner.h"

#include <algorithm>
#include <array>

#include "core/path_utf8.h"

namespace musicboard {
namespace {

// 一次扫描里"候选音频文件"的扩展名白名单。
// 只用来决定要不要打开看一眼；真正的容器判断交给 probe_container（按魔数）。
constexpr std::array<std::string_view, 7> kSupportedExtensions = {
    "ogg", "mp3", "wav", "flac", "m4a", "aac", "wma",
};

[[nodiscard]] std::string lowercase_ascii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

// 目录项排序：名字按字典序，让每次扫描的顺序稳定。
// 界面上的曲目顺序不该因为文件系统的返回顺序而变。
void sort_categories(std::vector<LibraryCategory>& categories) {
    std::sort(categories.begin(), categories.end(),
              [](const LibraryCategory& a, const LibraryCategory& b) { return a.name < b.name; });
    for (LibraryCategory& category : categories) {
        std::sort(category.tracks.begin(), category.tracks.end(),
                  [](const LibraryTrack& a, const LibraryTrack& b) { return a.name < b.name; });
    }
}

// 隐藏目录 / 文件一律跳过（.git、.DS_Store 之类）
[[nodiscard]] bool is_hidden(std::string_view name) {
    return !name.empty() && name.front() == '.';
}

// 分类目录也按名字排序。不排的话，下面"哪个目录先被扫"就取决于
// 文件系统的返回顺序 —— 而进度回调里的候选序号会跟着变。
void sort_folders(std::vector<std::filesystem::path>& folders) {
    std::sort(folders.begin(), folders.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                  return path_to_utf8(a.filename()) < path_to_utf8(b.filename());
              });
}

struct FolderScan final {
    std::vector<LibraryCategory> categories;
    std::vector<std::string> warnings;
    std::size_t candidate_count = 0;
    std::size_t loaded_count = 0;
    std::size_t failed_count = 0;
    bool cancelled = false;
};

// 扫一个分类目录（只一层，不递归）
//
// keep_when_empty：一个候选都没有时要不要照样交出这个分类。
//   用户自己建的分类 → true（新建的空分类要看得见，否则"建了没反应"）
//   曲目直接散落在栏目根下 → false（否则每张专辑旁边都挂一个空的"默认"）
void scan_category_folder(const std::filesystem::path& folder, const std::string& category_name,
                          const ScanOptions& options, bool keep_when_empty, FolderScan& out) {
    std::error_code ec;
    std::filesystem::directory_iterator it(folder, ec);
    if (ec) {
        out.warnings.push_back("无法打开分类目录 " + path_to_utf8(folder) + ": " + ec.message());
        return;
    }

    LibraryCategory category;
    category.name = category_name;
    bool saw_candidate = false;

    for (const auto& entry : it) {
        if (out.cancelled) {
            return;
        }
        if (options.should_cancel && options.should_cancel()) {
            out.cancelled = true;
            return;
        }

        const std::string file_name = path_to_utf8(entry.path().filename());
        if (is_hidden(file_name)) {
            continue;
        }
        if (!entry.is_regular_file(ec)) {
            continue;   // 子目录在这里被忽略 —— 只扫一层，见头文件的说明
        }

        const std::string extension = lowercase_ascii(path_to_utf8(entry.path().extension()));
        // extension 带点，去掉
        const std::string_view without_dot =
            extension.size() > 1 ? std::string_view(extension).substr(1) : std::string_view{};
        if (!is_supported_audio_extension(without_dot)) {
            continue;   // 不是候选音频，静默跳过（不该给用户报"这个 txt 读不了"）
        }

        saw_candidate = true;
        ++out.candidate_count;
        if (options.on_progress) {
            options.on_progress(out.candidate_count, 0, entry.path());
        }

        AudioInfo info;
        Failure failure;
        if (!probe_audio_file(entry.path(), info, failure)) {
            ++out.failed_count;
            // 逐条记下来。原版在这里只写控制台，用户看到的是"这首怎么没了"。
            out.warnings.push_back(file_name + " —— " + failure.to_string());
            continue;
        }

        LibraryTrack track;
        track.name = path_to_utf8(entry.path().stem());
        track.path = entry.path();
        track.container = info.container;
        track.format = info.format;
        track.duration_seconds = info.duration_seconds;
        category.tracks.push_back(std::move(track));
        ++out.loaded_count;
    }

    // 空分类也保留 —— 用户刚新建的分类需要看得见，否则"建了没反应"
    if (keep_when_empty || saw_candidate) {
        out.categories.push_back(std::move(category));
    }
}

// 忽略某个子目录名（Windows 文件名不区分大小写，所以比较也按 ASCII 折叠）
[[nodiscard]] bool name_equals_ignoring_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto left = static_cast<unsigned char>(a[i]);
        const auto right = static_cast<unsigned char>(b[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

// 扫一个栏目（BGM 或 SFX）。base 是该栏目的根目录。
//
// excluded_folder 用于 BGM 退让到曲库根目录的那种情况：此时 SFX 子目录
// 是**另一个栏目**，不能当成 BGM 的一个分类扫进来 ——
// 否则用户看到的是"音乐分类里多了一个叫 SFX 的项"，而他放在根目录的音乐一首都没有。
void scan_section(const std::filesystem::path& base, const ScanOptions& options,
                  std::string_view excluded_folder, FolderScan& out) {
    std::error_code ec;
    std::filesystem::directory_iterator it(base, ec);
    if (ec) {
        out.warnings.push_back("无法打开目录 " + path_to_utf8(base) + ": " + ec.message());
        return;
    }

    std::vector<std::filesystem::path> subfolders;
    for (const auto& entry : it) {
        if (out.cancelled) {
            return;
        }
        const std::string name = path_to_utf8(entry.path().filename());
        if (is_hidden(name)) {
            continue;
        }
        if (!excluded_folder.empty() && name_equals_ignoring_case(name, excluded_folder)) {
            continue;
        }
        if (entry.is_directory(ec)) {
            subfolders.push_back(entry.path());
        }
    }

    sort_folders(subfolders);

    if (subfolders.empty()) {
        // 没有分类子目录：这一层的文件就是"默认"分类。
        // 原版也是这个退让 —— 用户会把文件直接丢在 BGM 根下。
        scan_category_folder(base, std::string(default_category_name), options, true, out);
        return;
    }

    for (const auto& folder : subfolders) {
        if (out.cancelled) {
            return;
        }
        scan_category_folder(folder, path_to_utf8(folder.filename()), options, true, out);
    }

    // 有分类子目录、但栏目根下也散着几个音频文件的情况。
    //
    // 只扫一层的话这些文件会**彻底消失**：磁盘上明明在，界面上永远看不到，
    // 而且没有任何提示。实测真实曲库就有一个（BGM/背景恐怖音2.mp3）。
    // 处理办法是给它们一个"默认"分类，而不是丢掉或者报错 ——
    // 用户不用改目录结构，也不会以为文件被吃了。
    // 一个候选都没有时不要凭空多出这个分类（keep_when_empty=false）。
    if (!out.cancelled) {
        scan_category_folder(base, std::string(default_category_name), options, false, out);
    }
}

}  // namespace

std::size_t LibrarySnapshot::total_tracks() const noexcept {
    std::size_t total = 0;
    for (const auto& category : bgm) {
        total += category.tracks.size();
    }
    for (const auto& category : sfx) {
        total += category.tracks.size();
    }
    return total;
}

bool is_supported_audio_extension(std::string_view extension_lowercase) noexcept {
    return std::find(kSupportedExtensions.begin(), kSupportedExtensions.end(),
                     extension_lowercase) != kSupportedExtensions.end();
}

bool scan_library(const ScanOptions& options, LibrarySnapshot& snapshot, Failure& failure) {
    snapshot = LibrarySnapshot{};

    if (options.root.empty()) {
        failure.set(fail_code::invalid_argument, "曲库根目录为空");
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(options.root, ec)) {
        failure.set(fail_code::file_not_found,
                    "曲库根目录不存在或不是目录: " + path_to_utf8(options.root));
        return false;
    }

    // BGM 与 SFX 的定位规则（与原版一致）：
    //   有 <根>/BGM 就用它；没有就把根本身当 BGM 目录，
    //   但这时必须把 <根>/SFX 排除掉 —— 它是另一个栏目，不是 BGM 的分类。
    //   SFX 必须是 <根>/SFX，没有就没有 —— 不能把根也当 SFX，那会把音乐误当音效。
    const bool has_bgm_folder = std::filesystem::is_directory(options.root / L"BGM", ec);
    const std::filesystem::path bgm_base =
        has_bgm_folder ? options.root / L"BGM" : options.root;
    const std::filesystem::path sfx_base = options.root / L"SFX";
    const bool has_sfx = std::filesystem::is_directory(sfx_base, ec);

    FolderScan bgm_scan;
    scan_section(bgm_base, options, has_bgm_folder ? std::string_view{} : std::string_view("SFX"),
                 bgm_scan);
    if (bgm_scan.cancelled) {
        snapshot.cancelled = true;
        snapshot.warnings = std::move(bgm_scan.warnings);
        return true;
    }

    FolderScan sfx_scan;
    if (has_sfx) {
        scan_section(sfx_base, options, {}, sfx_scan);
        if (sfx_scan.cancelled) {
            snapshot.cancelled = true;
            snapshot.warnings = std::move(bgm_scan.warnings);
            return true;
        }
    }

    snapshot.bgm = std::move(bgm_scan.categories);
    snapshot.sfx = std::move(sfx_scan.categories);
    snapshot.warnings = std::move(bgm_scan.warnings);
    snapshot.warnings.insert(snapshot.warnings.end(), sfx_scan.warnings.begin(),
                             sfx_scan.warnings.end());
    snapshot.candidate_count = bgm_scan.candidate_count + sfx_scan.candidate_count;
    snapshot.loaded_count = bgm_scan.loaded_count + sfx_scan.loaded_count;
    snapshot.failed_count = bgm_scan.failed_count + sfx_scan.failed_count;

    sort_categories(snapshot.bgm);
    sort_categories(snapshot.sfx);

    if (options.on_progress) {
        options.on_progress(snapshot.loaded_count, snapshot.candidate_count, {});
    }
    return true;
}

}  // namespace musicboard
